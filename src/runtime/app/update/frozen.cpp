// frozen.cpp — append-only scrollback prefix.
//
// Mirrors the agent_session example's `m.frozen` discipline: settled
// turns are built into Element values ONCE at the moment they settle,
// pushed into m.ui.frozen, and rendered by maya via list_ref. The view
// (conversation_config) hands maya a borrowed pointer to this vector,
// so the per-frame cost is O(visible_live) regardless of how long the
// session runs — instead of O(visible_total_turns × tool_cards_per_turn).
//
// The producer is `freeze_through(m, live_start)`: walks
// messages[frozen_through .. live_start), applies the same tool-batch
// merge that conversation_config used to apply at view time, and pushes
// one Turn Element (preceded by a gap) per visual unit. Compaction
// dividers are inserted at their boundary indices.
//
// Lifecycle invariants:
//   • frozen.size() corresponds to messages[0 .. frozen_through).
//   • frozen entries are immutable once pushed (Element values are
//     read-only after construction; the underlying shared_ptr Element
//     caches inside view_cache may be evicted, but the snapshot copy
//     here keeps the rendered subtree alive).
//   • Any operation that mutates messages[i] for i < frozen_through is
//     forbidden; if such a mutation becomes necessary (checkpoint
//     restore, retroactive edit), call rehydrate_frozen() to rebuild
//     from scratch.

#include "agentty/runtime/app/update/internal.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include <maya/dsl.hpp>
#include <maya/render/cache_id.hpp>
#include <maya/widget/conversation.hpp>
#include <maya/widget/turn.hpp>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/turn.hpp"

namespace agentty::app::detail {

namespace {

// Thin dim ─ rule between turns. Pushed before each fresh-speaker
// turn so settled turns are visually separated.
maya::Element gap_row() {
    return maya::Conversation::divider();
}

// Compaction-boundary divider Element. Single-line `≡ Conversation
// compacted` rule, identical chrome to the inline-built version that
// conversation_config used to manufacture each frame.
maya::Element compaction_divider_row() {
    maya::Turn::Config cfg;
    cfg.glyph      = "\xe2\x89\xa1";   // ≡
    cfg.label      = "Conversation compacted";
    cfg.rail_color = ui::muted;
    return maya::Turn{std::move(cfg)}.build();
}

// Sentinel-check: is `mm` an Assistant message whose only content is
// tool_calls (no prose)? Retained as documentation of the
// classification; the actual run-merge policy now lives in
// `ui::turn_run_end`, called from both `freeze_range` and
// `build_live_tail`.
[[maybe_unused]] bool is_tool_only_assistant(const Message& mm) {
    return mm.role == Role::Assistant
        && mm.text.empty()
        && mm.streaming_text.empty()
        && !mm.tool_calls.empty();
}

// Run-level safety gate: a frozen turn captures an Element snapshot
// whose hash_id is stamped once and never recomputed. If we freeze a
// run that still contains a Pending / Approved / Running tool, that
// tool's status would mutate later (when ToolExecOutput finally
// lands) but the rendered Element in m.ui.frozen would keep the
// pre-mutation state forever — visible as a permanently-Running
// spinner in scrollback. Refuse to freeze any run that isn't fully
// terminal; the next freeze_through pass picks it up once the live
// path has settled it.
bool run_is_freezable(const Model& m, std::size_t from, std::size_t run_end) {
    for (std::size_t j = from; j < run_end; ++j) {
        for (const auto& tc : m.d.current.messages[j].tool_calls) {
            if (!tc.is_terminal()) return false;
        }
    }
    return true;
}

// Freeze messages[from .. to), pushing built Turn Elements (and any
// leading gap / compaction divider) into m.ui.frozen. One Turn per
// speaker-run: a User message is its own Turn; a run of consecutive
// Assistant messages collapses into ONE Turn whose body interleaves
// each sub-turn's text + tool batch (see
// `ui::turn_config_for_assistant_run`). The same `ui::turn_run_end`
// helper drives the boundary in `build_live_tail` so the frozen and
// live row shapes are identical for the same input.
//
// Advances `m.ui.frozen_turn` once per Assistant run (one logical
// agent turn equals one display number) so the running turn count
// the live tail will compute next stays in sync.
void freeze_range(Model& m, std::size_t from, std::size_t to) {
    const std::size_t total = m.d.current.messages.size();
    if (from >= to || to > total) return;

    auto needs_compaction_divider = [&](std::size_t i) {
        for (const auto& rec : m.d.current.compactions) {
            if (rec.up_to_index == i && rec.up_to_index > 0
                && rec.up_to_index <= total) return true;
        }
        return false;
    };

    std::size_t i = from;
    while (i < to) {
        // Run boundary — shared with build_live_tail.
        const std::size_t run_end_global =
            ui::turn_run_end(m.d.current.messages, i);
        const std::size_t run_end = std::min(run_end_global, to);

        // Safety gate: if any tool in this run is not yet terminal,
        // stop here. frozen_through advances to `i` (the start of
        // the un-freezable run) so the next freeze_through call
        // resumes here once the live path has settled it.
        if (!run_is_freezable(m, i, run_end)) {
            m.ui.frozen_through = i;
            return;
        }

        if (needs_compaction_divider(i)) {
            m.ui.frozen.push_back(compaction_divider_row());
        }

        // Leading gap: one blank row before every turn except the
        // very first frozen row (avoid a top-of-thread gap).
        const bool first_overall = m.ui.frozen.empty();
        if (!first_overall) {
            m.ui.frozen.push_back(gap_row());
        }

        const Message& head = m.d.current.messages[i];

        if (head.role == Role::Assistant) {
            int turn_num = m.ui.frozen_turn + 1;
            auto cfg = ui::turn_config_for_assistant_run(
                i, run_end, turn_num, m, /*synthetic=*/false);

            // Hash key: settled assistant run. The merge inputs (every
            // run-member msg.id + the run length) all fold in so a
            // different run produces a different key. Once frozen,
            // none of the underlying bytes change — the key is stable
            // for the lifetime of this entry, and maya's hash-keyed
            // ComponentCache reuses the painted cells every frame.
            maya::CacheIdBuilder kb;
            kb.add(std::string_view{"agentty.turn.assistant_run"})
              .add(static_cast<std::uint64_t>(run_end - i));
            for (std::size_t j = i; j < run_end; ++j) {
                kb.add(std::string_view{m.d.current.messages[j].id.value});
                kb.add(m.d.current.messages[j].compute_render_key());
            }
            cfg.hash_id = kb.build();
            m.ui.frozen.push_back(maya::Turn{std::move(cfg)}.build());
            ++m.ui.frozen_turn;
        } else {
            // User / compaction-summary single-message Turn.
            int turn_num = m.ui.frozen_turn;
            auto cfg = ui::turn_config(head, i, turn_num, m,
                                       /*continuation=*/false,
                                       /*synthetic=*/false);
            cfg.hash_id = maya::CacheIdBuilder{}
                .add(std::string_view{"agentty.turn"})
                .add(std::string_view{head.id.value})
                .add(head.compute_render_key())
                .build();
            m.ui.frozen.push_back(maya::Turn{std::move(cfg)}.build());
        }

        i = run_end;
    }

    m.ui.frozen_through = to;
}

} // namespace

void freeze_through(Model& m, std::size_t live_start) {
    if (live_start <= m.ui.frozen_through) return;
    freeze_range(m, m.ui.frozen_through, live_start);
}

void clear_frozen(Model& m) {
    m.ui.frozen.clear();
    m.ui.frozen_through = 0;
    m.ui.frozen_turn    = 0;
}

void rehydrate_frozen(Model& m) {
    clear_frozen(m);
    const auto& msgs = m.d.current.messages;
    const std::size_t total = msgs.size();
    if (total == 0) return;

    // Bounded rehydrate. Two costs scale with the rehydrated tail:
    //
    //   1. Build cost: markdown parse + tool body preview + timeline
    //      layout, paid once per frozen Element at rehydrate time.
    //   2. Paint cost on the swap frame: every canvas row gets emitted
    //      to the wire row-by-row with \r\n between, and each \r\n at
    //      the viewport bottom edge scrolls the terminal one row. Rows
    //      that scroll past the top of the viewport during this paint
    //      are NOT in native scrollback (they were never live in this
    //      session) — pure wasted bytes that the user sees as the
    //      "paint from top, fast-scroll to bottom" lag on thread
    //      resume.
    //
    // Two caps in conjunction:
    //   • Turn cap (kRehydrateTurns) bounds the build cost on
    //     turn-heavy threads where each turn is small.
    //   • Row budget (kRehydrateRowBudget) bounds the paint cost on
    //     content-heavy threads where ONE turn (e.g. a Write of a
    //     2000-line file) can blow past the budget by itself.
    //
    // Either cap stops the backward walk. The row estimate is bytes
    // / 60 (rough "average rendered chars per row" across prose,
    // code, tool bodies) — deliberately conservative so we under-
    // count and stop early rather than over-emit. Older turns live
    // in the on-disk JSON (m.d.current.messages is intact); they're
    // just invisible inside agentty until the next live append
    // shifts the window. Composer history (↑ in the composer) still
    // walks every prior user prompt, so the recall path is unaffected.
    constexpr std::size_t kRehydrateTurns     = 6;
    constexpr std::size_t kRehydrateRowBudget = 200;   // ~3-8 viewports

    auto estimate_msg_rows = [](const Message& mm) -> std::size_t {
        std::size_t bytes = mm.text.size() + mm.streaming_text.size();
        for (const auto& tc : mm.tool_calls) {
            bytes += tc.output().size();
            bytes += tc.args_streaming.size();
            // Header / footer / chrome rows per tool card (~4 rows
            // even for an empty body — title, divider, status, blank).
            bytes += 4 * 60;
        }
        // Per-message envelope (header, gap, divider).
        bytes += 3 * 60;
        return bytes / 60 + 1;
    };

    // Walk backward counting speaker-runs until EITHER cap trips.
    std::size_t units      = 0;
    std::size_t row_budget = 0;
    std::size_t start      = total;
    std::size_t cursor     = total;
    while (cursor > 0) {
        std::size_t j = cursor;
        if (msgs[j - 1].role == Role::Assistant) {
            while (j > 0 && msgs[j - 1].role == Role::Assistant) --j;
        } else {
            --j;
        }
        // Estimate rows for THIS run before committing to include it.
        std::size_t run_rows = 0;
        for (std::size_t k = j; k < cursor; ++k)
            run_rows += estimate_msg_rows(msgs[k]);
        // Always include at least one run — even a giant one is what
        // the user just loaded and wants to see. Stop AFTER the run
        // that pushes us over budget so the most-recent context is
        // always visible.
        ++units;
        start = j;
        row_budget += run_rows;
        if (units >= kRehydrateTurns) break;
        if (row_budget >= kRehydrateRowBudget) break;
        cursor = j;
    }

    // Seed frozen_turn to the count of assistant runs in the skipped
    // prefix so visible turn numbers reflect their true position
    // (e.g. "turn 87" not "turn 1" on a long reload).
    int skipped_assistant_runs = 0;
    for (std::size_t k = 0; k < start; ) {
        const std::size_t run_end = ui::turn_run_end(msgs, k);
        if (msgs[k].role == Role::Assistant) ++skipped_assistant_runs;
        k = run_end;
    }
    m.ui.frozen_turn = skipped_assistant_runs;

    freeze_range(m, start, total);
}

maya::Cmd<Msg> trim_frozen_if_oversized(Model& m) {
    // Soft cap on the frozen vector. Above this, the oldest entries
    // are dropped — maya's row diff sees a shorter live tree and the
    // already-overflowed rows naturally commit to native scrollback.
    //
    // Tradeoff: memory + every-frame render_tree cost vs in-app
    // scroll reach. Render cost dominates on tool-heavy sessions —
    // every settled turn appends a multi-row Element to frozen and
    // the canvas auto-resizes to `total_rows + 8`. canvas_.clear()
    // streaming_fills the entire surface each frame and render_tree
    // walks every node to position it; 240 entries of write/edit/bash
    // panels reaches ~5000 rows and pushes per-frame render past
    // 15 ms, which the user feels as input lag.
    //
    // 80 entries ≈ 25-30 full turns of recent work — enough for the
    // in-flight task to stay visible, small enough that the canvas
    // never blows past ~2000 rows. Older turns are still in the
    // terminal's native scrollback (committed there when they
    // overflowed during the live session). 30-entry trim chunk
    // amortises the per-trim cost across many appends.
    constexpr std::size_t kFrozenMax  = 80;
    constexpr std::size_t kFrozenTrim = 30;

    if (m.ui.frozen.size() <= kFrozenMax) return maya::Cmd<Msg>::none();

    const std::size_t n = std::min(kFrozenTrim,
        m.ui.frozen.size() > kFrozenMax / 2
            ? m.ui.frozen.size() - kFrozenMax / 2
            : std::size_t{0});
    if (n == 0) return maya::Cmd<Msg>::none();

    m.ui.frozen.erase(m.ui.frozen.begin(),
                      m.ui.frozen.begin() + static_cast<std::ptrdiff_t>(n));

    // commit_scrollback_overflow lets maya derive the safe row count
    // itself (max(0, prev_rows - term_h)) — the Cmd is just a trigger
    // saying "please release whatever has already overflowed." This
    // is the safe variant; the row-counted commit_scrollback was
    // retired in the maya audit (see scrollback-corruption-audit.md
    // finding #1) because no caller outside the renderer can know
    // the right physical-row count.
    return maya::Cmd<Msg>::commit_scrollback_overflow();
}

} // namespace agentty::app::detail
