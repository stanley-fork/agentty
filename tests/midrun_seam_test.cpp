// midrun_seam_test — scrollback-duplication regression for the
// incremental mid-run freeze.
//
// The real symptom users hit: during a long auto-pilot run, sub-turns
// are frozen one at a time (freeze_settled_subturns fires on each
// ToolExecOutput). maya's inline diff treats any row that has scrolled
// past the viewport top as IMMUTABLE — already committed to the
// terminal's native scrollback, unrewritable. So the rendering
// contract is: once a row is emitted at canvas position Y, the row at
// position Y must NEVER change in a later frame (until a full
// Divergent repaint). If freezing a sub-turn shifts the rows above it
// by even one (a gap appears/disappears, a header row toggles), every
// already-committed row below the shift is re-emitted at a new
// position — and the old copy is stuck in scrollback. That's the
// duplicated-turn ghost.
//
// This test simulates the exact incremental cadence and asserts the
// rendered row PREFIX is stable across every freeze step: frame N+1's
// rows [0..K) must byte-match frame N's rows [0..K) where K is the
// shorter common prefix that was already on screen. Any divergence in
// the committed prefix is a duplication bug.

#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/app_layout.hpp>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/changes_strip.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"
#include "agentty/runtime/view/thread/conversation.hpp"
#include "agentty/runtime/view/thread/thread.hpp"

using agentty::Model;
using agentty::Message;
using agentty::Role;
using agentty::ToolCallId;
using agentty::ToolName;
using agentty::ToolUse;
using std::chrono::steady_clock;
using std::chrono::milliseconds;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::fprintf(stderr, "  FAIL [%s:%d] %s\n",                    \
                         __FILE__, __LINE__, (msg));                       \
        }                                                                  \
    } while (0)

static std::string code_block(int n) {
    std::string out;
    for (int i = 0; i < n; ++i)
        out += "    auto x = compute(i) + offset; // plausible line\n";
    return out;
}

static ToolUse settled_edit(const std::string& tag) {
    ToolUse t;
    t.id   = ToolCallId{"edit_" + tag};
    t.name = ToolName{"edit"};
    nlohmann::json edits = nlohmann::json::array();
    edits.push_back({{"old_text", code_block(4)},
                     {"new_text", code_block(4)}});
    t.args = {{"path", "src/" + tag + ".cpp"}, {"edits", edits}};
    auto now = steady_clock::now();
    t.status = ToolUse::Done{now - milliseconds{5}, now, "edited-" + tag};
    return t;
}

// Render the CONVERSATION region only (frozen + live tail), one row per
// line, ASCII-folded, trailing blanks stripped.
static std::vector<std::string> render_rows(const Model& m,
                                            int width = 100,
                                            int height = 40000) {
    auto root = maya::AppLayout{{
        .thread        = agentty::ui::thread_config(m),
        .changes_strip = agentty::ui::changes_strip_config(m),
        .composer      = agentty::ui::composer_config(m),
        .status_bar    = agentty::ui::status_bar_config(m),
        .overlay       = std::nullopt,
    }}.build();

    maya::StylePool pool;
    maya::Canvas canvas(width, height, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::dark, true);

    std::vector<std::string> rows;
    const int max_row = canvas.max_content_row();
    for (int y = 0; y <= max_row; ++y) {
        std::string line;
        for (int x = 0; x < width; ++x) {
            char32_t ch = canvas.get(x, y).character;
            if (ch == 0) ch = U' ';
            line.push_back(ch < 128 ? static_cast<char>(ch) : '?');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        rows.push_back(std::move(line));
    }
    return rows;
}

// Compare two frames under maya's inline-overflow contract. Only rows
// that have scrolled ABOVE the viewport top are committed to native
// scrollback and immutable; on-screen rows (the bottom `term_h` of the
// canvas) may be freely rewritten frame-to-frame.
//
// A row at index i in frame `prev` was committed iff it had already
// overflowed: i < prev.size() - term_h. For the duplication bug we only
// care about those — if a committed row's content differs at the same
// index in `cur`, the renderer would re-emit it below a row already
// stuck in scrollback. Returns the first diverging COMMITTED row index,
// or -1 if every committed row is stable.
static int first_committed_divergence(const std::vector<std::string>& prev,
                                      const std::vector<std::string>& cur,
                                      int term_h) {
    // Rows of `prev` that had overflowed the viewport last frame.
    const int committed_end =
        static_cast<int>(prev.size()) - term_h;
    const int common =
        std::min<int>(static_cast<int>(cur.size()), committed_end);
    for (int i = 0; i < common; ++i)
        if (prev[static_cast<std::size_t>(i)] != cur[static_cast<std::size_t>(i)])
            return i;
    return -1;
}

// Build the User + N settled edit sub-turns, freeze the User, then
// replay the incremental freeze cadence: after each sub-turn settles,
// call freeze_settled_subturns and snapshot the rendered rows. Assert
// the committed prefix never changes between consecutive snapshots.
static void test_incremental_freeze_prefix_stable() {
    constexpr int N = 30;
    // A realistic viewport: maya keeps the bottom `term_h` rows on
    // screen; everything above has overflowed into native scrollback
    // and is immutable. Use a typical 40-row terminal. With N=30 edit
    // sub-turns (~6 rows each) the canvas grows well past 40 rows, so
    // early sub-turns genuinely commit to scrollback — exactly the
    // condition under which a prefix shift becomes a visible duplicate.
    constexpr int kTermH = 40;

    Model m;
    m.d.current.id = agentty::ThreadId{"seam"};
    Message u; u.role = Role::User; u.text = "please do many edits";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);   // User frozen

    std::vector<std::string> prev;
    int worst_divergence = -1;
    int diverging_step   = -1;

    for (int e = 0; e < N; ++e) {
        // A settled edit sub-turn lands.
        Message a; a.role = Role::Assistant;
        a.tool_calls.push_back(settled_edit("s" + std::to_string(e)));
        m.d.current.messages.push_back(std::move(a));

        // A streaming successor placeholder keeps the just-landed
        // sub-turn freezable while the run stays active (exactly what
        // the post-tool continuation pushes in the real reducer).
        Message ph; ph.role = Role::Assistant; ph.streaming_text = "working";
        m.d.current.messages.push_back(std::move(ph));
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

        // Mid-run freeze ONLY, mirroring ToolExecOutput in tool.cpp.
        // Trimming is deferred to turn boundaries (stream finish /
        // submit), so it must NOT run here.
        agentty::app::detail::freeze_settled_subturns(m);

        auto cur = render_rows(m);
        if (!prev.empty()) {
            int d = first_committed_divergence(prev, cur, kTermH);
            if (d >= 0 && (worst_divergence < 0 || d < worst_divergence)) {
                worst_divergence = d;
                diverging_step   = e;
                // Dump the neighbourhood of the FIRST divergence so a
                // regression shows exactly which committed row moved.
                std::fprintf(stderr,
                    "  --- step %d, committed-row divergence at row %d ---\n",
                    e, d);
                for (int y = std::max(0, d);
                     y < std::min<int>(d + 3,
                         (int)std::max(prev.size(), cur.size()));
                     ++y) {
                    const char* pv = (y < (int)prev.size()) ? prev[y].c_str() : "<none>";
                    const char* cv = (y < (int)cur.size())  ? cur[y].c_str()  : "<none>";
                    std::fprintf(stderr, "    row %2d PREV |%s|\n", y, pv);
                    std::fprintf(stderr, "    row %2d CUR  |%s|\n", y, cv);
                }
            }
        }
        prev = std::move(cur);

        // Drop the placeholder before the next real sub-turn (the live
        // tail is rebuilt each cycle; the next edit message replaces it).
        m.d.current.messages.pop_back();
    }

    if (worst_divergence >= 0) {
        std::fprintf(stderr,
            "  committed prefix changed at row %d on freeze step %d "
            "(a scrollback row was rewritten -> duplication)\n",
            worst_divergence, diverging_step);
    }
    CHECK(worst_divergence < 0,
          "committed prefix mutated across an incremental freeze");
}

int main() {
    std::printf("midrun_seam_test\n");
    test_incremental_freeze_prefix_stable();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) { std::printf("FAILED\n"); return 1; }
    std::printf("PASSED\n");
    return 0;
}
