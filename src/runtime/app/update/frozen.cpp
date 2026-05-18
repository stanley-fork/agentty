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
// tool_calls (no prose)? Used by the tool-batch merge in freeze_range.
bool is_tool_only_assistant(const Message& mm) {
    return mm.role == Role::Assistant
        && mm.text.empty()
        && mm.streaming_text.empty()
        && !mm.tool_calls.empty();
}

// Freeze messages[from .. to), pushing built Turn Elements (and any
// leading gap / compaction divider) into m.ui.frozen. Mirrors the
// tool-batch-merge logic from the legacy view path: a run of
// consecutive tool-only Assistant continuations collapses into ONE
// merged Turn whose tool_calls are the union of the run's.
//
// Advances m.ui.frozen_turn on each fresh-speaker assistant turn so
// the running turn number matches what the live tail will compute
// next.
void freeze_range(Model& m, std::size_t from, std::size_t to) {
    const std::size_t total = m.d.current.messages.size();
    if (from >= to || to > total) return;

    // Build a set of message indices that should be PRECEDED by a
    // compaction divider. up_to_index is "covers messages[0..N)" —
    // the divider sits immediately before messages[N].
    auto needs_compaction_divider = [&](std::size_t i) {
        for (const auto& rec : m.d.current.compactions) {
            if (rec.up_to_index == i && rec.up_to_index > 0
                && rec.up_to_index <= total) return true;
        }
        return false;
    };

    for (std::size_t i = from; i < to; ++i) {
        if (needs_compaction_divider(i)) {
            m.ui.frozen.push_back(compaction_divider_row());
        }

        const auto& msg = m.d.current.messages[i];

        const bool continuation =
            (msg.role == Role::Assistant) &&
            (i > 0) &&
            (m.d.current.messages[i - 1].role == Role::Assistant);

        // Tool-batch merge: collapse a run of tool-only Assistant
        // continuations into the head message's panel. See
        // conversation.cpp's live-tail path for the full rationale —
        // we replicate it here so the frozen visual matches what the
        // user saw during the live phase.
        //
        // Head eligibility is any non-empty Assistant message (text
        // OR tool_calls). Without this, a `text reply → tool call`
        // two-message turn freezes as two Turns where the second
        // gets continuation=true and loses its header banner.
        const bool head_mergeable =
            msg.role == Role::Assistant
            && (!msg.text.empty() || !msg.streaming_text.empty()
                || !msg.tool_calls.empty());
        std::size_t run_end = i + 1;
        if (head_mergeable) {
            while (run_end < to
                   && m.d.current.messages[run_end].role == Role::Assistant
                   && is_tool_only_assistant(m.d.current.messages[run_end])) {
                ++run_end;
            }
        }

        // Leading gap: one blank row before every fresh-speaker turn
        // (skip on the very first frozen row to avoid a top-of-thread
        // gap, and skip before continuations so a same-speaker run
        // visually flows).
        const bool first_overall = m.ui.frozen.empty();
        if (!first_overall && !continuation) {
            m.ui.frozen.push_back(gap_row());
        }

        if (run_end > i + 1) {
            // Merged run: head message + appended tool_calls from the
            // continuation tail. We pass `msg` through unchanged and
            // hand turn_config a borrowed span of the union via the
            // tool_calls_override path — no Message-sized deep copy,
            // no per-frame ToolUse churn beyond the one allocation
            // for the merged vector. The originals live in
            // `m.d.current.messages[*]` and the span borrows from
            // them; the vector outlives the turn_config / Turn::build
            // call.
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
            int turn_num = m.ui.frozen_turn + 1;
            auto cfg = ui::turn_config(msg, i, turn_num, m,
                                       continuation, /*synthetic=*/true,
                                       /*meta_override=*/{},
                                       /*tool_calls_override=*/merged_tools);
            // Hash key: settled merged turn. The merge inputs (head
            // msg.id + every tail msg.id + tail count) all fold in so
            // a different run produces a different key. Once frozen,
            // the message bytes never change — the key is stable for
            // the lifetime of this entry, and maya's hash-keyed
            // ComponentCache reuses the painted cells every frame.
            maya::CacheIdBuilder kb;
            kb.add(std::string_view{"agentty.turn.merged"})
              .add(std::string_view{msg.id.value})
              .add(static_cast<std::uint64_t>(run_end - i));
            for (std::size_t j = i; j < run_end; ++j)
                kb.add(std::string_view{m.d.current.messages[j].id.value});
            cfg.hash_id = kb.build();
            m.ui.frozen.push_back(maya::Turn{std::move(cfg)}.build());
            if (msg.role == Role::Assistant && !continuation)
                ++m.ui.frozen_turn;
            i = run_end - 1;   // for-loop ++ lands on run_end
            continue;
        }

        int turn_num = m.ui.frozen_turn
                     + ((msg.role == Role::Assistant && !continuation) ? 1 : 0);
        // Build directly — no turn_element / shared_ptr wrapper. The
        // raw Element goes straight into m.ui.frozen, identical in
        // shape to agent_session's `m.frozen.push_back(widget.build())`.
        auto cfg = ui::turn_config(msg, i, turn_num, m, continuation,
                                   /*synthetic=*/true);
        // Hash key: settled single-message turn. msg.id is unique
        // per Message and stable across reloads; compute_render_key()
        // folds every visible field (text, tool_calls, error, etc.)
        // so even a retroactive mutation that bumps it invalidates
        // the cache. For frozen turns the key is effectively stable
        // for the lifetime of the entry.
        cfg.hash_id = maya::CacheIdBuilder{}
            .add(std::string_view{"agentty.turn"})
            .add(std::string_view{msg.id.value})
            .add(msg.compute_render_key())
            .build();
        m.ui.frozen.push_back(maya::Turn{std::move(cfg)}.build());
        if (msg.role == Role::Assistant && !continuation)
            ++m.ui.frozen_turn;
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
    // Default rehydration policy: freeze the entire history. The live
    // tail starts empty; the next stream (or composer action) decides
    // what becomes live. This matches the post-load expectation that
    // the user sees their full saved transcript as immutable scrollback,
    // with the composer poised to start a new turn beneath it.
    freeze_range(m, 0, m.d.current.messages.size());
}

maya::Cmd<Msg> trim_frozen_if_oversized(Model& m) {
    // Soft cap on the frozen vector. Above this, the oldest entries
    // are dropped — maya's row diff sees a shorter live tree and the
    // already-overflowed rows naturally commit to native scrollback.
    // The exact value is a trade-off between (a) memory footprint of
    // the cached Element subtrees + maya's prev_cells mirror, and
    // (b) how far back Ctrl+L / mouse-wheel can scroll within the
    // live frame before falling into native scrollback (which is
    // still visible, just not addressable by the renderer's diff).
    //
    // 240 entries ≈ 60-80 full turns; 80-entry trim chunk amortises
    // the per-trim work over many turns. Mirrors agent_session's
    // FROZEN_MAX=240 / FROZEN_TRIM=80 constants.
    constexpr std::size_t kFrozenMax  = 240;
    constexpr std::size_t kFrozenTrim = 80;

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
