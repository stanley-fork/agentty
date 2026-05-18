// conversation.cpp — view adapter for the conversation viewport.
//
// agent_session-style fast path: hand maya a borrowed pointer to
// m.ui.frozen (the append-only built-Element vector that grows on
// every settled turn) plus a small live-tail of unfrozen Elements
// (the in-flight assistant turn + any queued-message previews).
//
// The per-frame cost is therefore O(visible_live_tail) regardless of
// how long the session has run. Settled turns are NEVER rebuilt:
// they were built into Element values inside m.ui.frozen at the
// moment they settled (see src/runtime/app/update/frozen.cpp) and
// stay there until thread switch / NewThread / compaction triggers
// a rebuild.

#include "agentty/runtime/view/thread/conversation.hpp"

#include <chrono>
#include <cstddef>
#include <utility>

#include <maya/dsl.hpp>
#include <maya/widget/activity_indicator.hpp>
#include <maya/widget/conversation.hpp>
#include <maya/widget/turn.hpp>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/turn.hpp"

namespace agentty::ui {

namespace {

// Thin dim ─ rule — same as the one frozen.cpp interleaves between
// settled turns, used between live-tail turns.
maya::Element gap_row() {
    return maya::Conversation::divider();
}

// Leading separator between the frozen prefix and the first live
// turn. Historically this was a 3-row sandwich (blank + divider +
// blank) to defend against the divider's width-aware component
// resolving to blank() on the w==0 first frame. That defense had a
// cost: when the in-flight turn later moved into m.ui.frozen,
// freeze_range replaced this 3-row seam with the 1-row gap_row(),
// shrinking the transcript by 2 rows in a single frame. The inline
// renderer's diff can repaint cleanly, but rows that had already
// scrolled into native scrollback during streaming retain the OLD
// (3-row-seam) layout, surfacing as a ghost composer / duplicate
// header at the scrollback↔viewport seam on the next keystroke.
//
// Symmetry with frozen.cpp's gap_row() eliminates the height shift
// at the freeze instant. The w==0 first-frame risk is benign — maya
// re-renders at proper width on the next frame anyway.
maya::Element leading_seam_row() {
    return maya::Conversation::divider();
}

// Sentinel-check: assistant message whose only content is tool_calls
// (no prose). Mirrors freeze_range's predicate so live and frozen
// apply the same batch-merge policy.
bool is_tool_only_assistant(const Message& mm) {
    return mm.role == Role::Assistant
        && mm.text.empty()
        && mm.streaming_text.empty()
        && !mm.tool_calls.empty();
}

// Build the live-tail Elements. Mirrors freeze_range's shape so the
// live visual matches what eventually lands in m.ui.frozen:
//   • One leading separator before each fresh-speaker turn (skipped
//     for the very first row of a fresh thread).
//   • A run of consecutive tool-only Assistant continuations
//     collapses into ONE merged Turn whose tool_calls are the union
//     of the run's. Without this the live tail stacks N ACTIONS
//     panels back-to-back with no separator (continuation suppresses
//     the gap), which is the bug the screenshot captures.
//
// Appends directly into `out` so the caller's destination vector
// (cfg.live_tail) receives each Element via push_back/std::move with
// no intermediate copy or staging vector.
void build_live_tail(const Model& m, int& running_turn,
                     std::vector<maya::Element>& out) {
    const std::size_t total = m.d.current.messages.size();
    const std::size_t start = std::min(m.ui.frozen_through, total);
    if (start >= total) return;

    out.reserve(out.size() + (total - start) * 2);

    bool first_in_tail = true;
    for (std::size_t i = start; i < total; ++i) {
        const auto& msg = m.d.current.messages[i];

        const bool empty_placeholder =
            msg.role == Role::Assistant
            && msg.text.empty()
            && msg.streaming_text.empty()
            && msg.tool_calls.empty();

        const bool continuation =
            (msg.role == Role::Assistant) &&
            (i > 0) &&
            (m.d.current.messages[i - 1].role == Role::Assistant);

        const bool need_gap = !continuation
            && !(first_in_tail && m.ui.frozen.empty() && i == 0);
        if (need_gap) {
            // First seam off the frozen prefix gets the padded variant
            // so the rule isn't crushed against the previous Turn's
            // bottom chrome or lost to a w==0 first frame. Subsequent
            // intra-tail seams use the plain one-row divider.
            out.push_back(first_in_tail && !m.ui.frozen.empty()
                              ? leading_seam_row()
                              : gap_row());
        }
        first_in_tail = false;

        // Tool-batch merge: collapse a run of tool-only Assistant
        // continuations into the head message's panel. Identical
        // policy to freeze_range — keeps the visual stable across
        // the live→frozen transition.
        //
        // Head eligibility is any non-empty Assistant message (text,
        // streaming text, or tool_calls). Without including text-only
        // heads, a `text reply → tool call` two-message turn renders
        // as two Turns where the second is flagged continuation and
        // loses its `> Opus` header banner.
        const bool head_mergeable =
            msg.role == Role::Assistant
            && (!msg.text.empty() || !msg.streaming_text.empty()
                || !msg.tool_calls.empty());
        std::size_t run_end = i + 1;
        if (head_mergeable) {
            while (run_end < total
                   && m.d.current.messages[run_end].role == Role::Assistant
                   && is_tool_only_assistant(m.d.current.messages[run_end])) {
                ++run_end;
            }
        }

        int turn_num = running_turn;

        if (run_end > i + 1) {
            // Tool-batch merge for the live tail: build a borrowed-span
            // union of the run's tool_calls and pass it to turn_config
            // via the override. No per-frame Message deep copy — the
            // head Message's `text`, `streaming_text`, `attachments`
            // etc. stay borrowed in place; only the (necessarily
            // copied) ToolUse elements land in a fresh local vector.
            std::vector<ToolUse> merged_tools;
            std::size_t reserve_n = msg.tool_calls.size();
            for (std::size_t j = i + 1; j < run_end; ++j)
                reserve_n += m.d.current.messages[j].tool_calls.size();
            merged_tools.reserve(reserve_n);
            merged_tools.insert(merged_tools.end(),
                                msg.tool_calls.begin(), msg.tool_calls.end());
            for (std::size_t j = i + 1; j < run_end; ++j) {
                const auto& src = m.d.current.messages[j].tool_calls;
                merged_tools.insert(merged_tools.end(),
                                    src.begin(), src.end());
            }
            auto cfg = turn_config(msg, i, turn_num, m, continuation,
                                   /*synthetic=*/true,
                                   /*meta_override=*/{},
                                   /*tool_calls_override=*/merged_tools);
            out.push_back(maya::Turn{std::move(cfg)}.build());
            if (msg.role == Role::Assistant && !continuation) ++running_turn;
            i = run_end - 1;   // for-loop ++ lands on run_end
            continue;
        }

        auto cfg = turn_config(msg, i, turn_num, m, continuation,
                               /*synthetic=*/true);
        // Empty placeholder: inject an animated activity indicator so
        // the Turn has visible, MOVING content from submit through
        // first delta. Uses the same m.s.spinner the bottom-bar
        // indicator advances on each tick (see meta.cpp), so the
        // glyph cycles every frame instead of sitting as a static
        // "… thinking" string.
        if (empty_placeholder) {
            using namespace maya::dsl;
            maya::ActivityIndicator::Config ind;
            ind.edge_color    = cfg.rail_color;
            ind.spinner_glyph = std::string{m.s.spinner.current_frame()};
            ind.label         = "thinking";
            cfg.body.emplace_back(
                maya::ActivityIndicator{std::move(ind)}.build());
        }
        out.push_back(maya::Turn{std::move(cfg)}.build());
        if (msg.role == Role::Assistant && !continuation) ++running_turn;
    }
}

// Build the queued-message preview rows: visible at the tail of the
// transcript so the user can see what's queued. Mirrors Claude
// Code's appearance at offset 80106500 — visually identical to real
// user turns; the "queued not sent" cue is absence-of-assistant +
// the composer's `❚ N queued` chip.
void build_queued_previews(const Model& m, int& running_turn,
                           std::vector<maya::Element>& out) {
    if (m.ui.composer.queued.empty()) return;
    out.reserve(out.size() + m.ui.composer.queued.size() * 2);
    auto now = std::chrono::system_clock::now();
    const std::size_t base_idx = m.d.current.messages.size();
    for (std::size_t qi = 0; qi < m.ui.composer.queued.size(); ++qi) {
        Message synthetic;
        synthetic.role        = Role::User;
        synthetic.text        = m.ui.composer.queued[qi].text;
        synthetic.attachments = m.ui.composer.queued[qi].attachments;
        synthetic.timestamp   = now;
        std::string meta = "queued #" + std::to_string(qi + 1)
                         + " / "     + std::to_string(m.ui.composer.queued.size());
        if (static_cast<int>(qi) == m.ui.composer.queue_peek_idx)
            meta = "\xe2\x9c\x8e editing \xe2\x80\x94 " + meta;   // ✎
        out.push_back(gap_row());
        auto cfg = turn_config(synthetic, base_idx + qi, running_turn, m,
                               /*continuation=*/false,
                               /*synthetic=*/true,
                               /*meta_override=*/meta);
        out.push_back(maya::Turn{std::move(cfg)}.build());
        ++running_turn;
    }
}

} // namespace

maya::Conversation::Config conversation_config(const Model& m) {
    maya::Conversation::Config cfg;

    // ── Borrowed frozen prefix (zero-copy). ─────────────────────────
    // maya renders this through list_ref, so growing m.ui.frozen does
    // not increase per-frame cost. Maya's hash_id-keyed cell cache
    // makes already-painted Elements hit on every subsequent frame.
    cfg.frozen = &m.ui.frozen;

    // ── Live tail. ──────────────────────────────────────────────────
    // The only thing rebuilt per frame. Bounded by one in-flight
    // agent turn (one User + possibly several Assistant continuations)
    // plus any queued-message previews.
    int running_turn = m.ui.frozen_turn + 1;
    build_live_tail(m, running_turn, cfg.live_tail);
    build_queued_previews(m, running_turn, cfg.live_tail);

    // No separate in_flight indicator — the empty-placeholder
    // assistant Turn carries its own "thinking…" body slot during
    // streaming (see build_live_tail), matching agent_session where
    // m.thinking_active produces a body slot inside the assistant
    // Turn rather than a free-floating indicator below it.
    cfg.in_flight = std::nullopt;
    return cfg;
}

} // namespace agentty::ui
