#pragma once
// agentty::app::Program — the maya runtime binding.
//
// Forwards to the per-domain reducer / view / subscribe.  The init function
// reads settings + recent threads through the Store seam.

#include <chrono>
#include <cstdint>

#include <maya/maya.hpp>

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/view/view.hpp"

namespace agentty::app {

[[nodiscard]] std::pair<Model, maya::Cmd<Msg>> init();

struct AgenttyApp {
    using Model = ::agentty::Model;
    using Msg   = ::agentty::Msg;

    static std::pair<Model, maya::Cmd<Msg>> init() { return ::agentty::app::init(); }

    static auto update(Model m, Msg msg) -> std::pair<Model, maya::Cmd<Msg>> {
        return ::agentty::app::update(std::move(m), std::move(msg));
    }

    static maya::Element view(const Model& m) {
        return ::agentty::ui::view(m);
    }

    static auto subscribe(const Model& m) -> maya::Sub<Msg> {
        return ::agentty::app::subscribe(m);
    }

    // Optional Program hook (see maya/app/app.hpp — detail::HasVisualHash).
    // The runtime calls this just before view(); when the hash is
    // unchanged from the previous render, view() + render() are skipped
    // entirely. Captures the axes that affect what the user can see;
    // intentionally omits things like Session::last_tick that change
    // every Tick without changing pixels.
    //
    // Time-driven animations (cursor blink, streaming caret pulse,
    // spinner) are bucketed at coarse intervals so each visible step
    // advances the hash. The bucket size is the upper bound on how
    // often we'll render purely for animation.
    static std::uint64_t visual_hash(const Model& m) {
        std::uint64_t k = 1469598103934665603ULL;
        auto mix = [&](std::uint64_t v) {
            k = (k ^ v) * 1099511628211ULL;
        };
        auto mix_str = [&](std::string_view s) {
            mix(s.size());
            // Don't hash every byte for long strings — a 50 KB tool
            // output would dominate the per-frame budget. Sample a
            // few stable offsets; combined with size + render_keys
            // computed elsewhere it's enough to detect change.
            if (!s.empty()) {
                mix(static_cast<std::uint8_t>(s.front()));
                mix(static_cast<std::uint8_t>(s.back()));
                if (s.size() >= 16)
                    mix(static_cast<std::uint8_t>(s[s.size() / 2]));
            }
        };

        // ── Domain.
        //
        // The frozen prefix — messages[0 .. ui.frozen_through) — is
        // an immutable archaeology layer (see m.ui.frozen). Its
        // contribution to the visual is fully captured by the
        // structural pair (frozen.size(), frozen_turn) which advance
        // only at freeze instants. Per-message render_keys for the
        // frozen range cannot change — the with_live_tool gate
        // refuses to mutate them — so iterating them every frame
        // would burn CPU for zero signal. Hash only the live tail.
        mix(m.d.current.messages.size());
        mix(static_cast<std::uint64_t>(m.ui.frozen.size()));
        mix(static_cast<std::uint64_t>(m.ui.frozen_turn));
        mix(m.ui.frozen_midrun ? 1ULL : 0ULL);
        for (std::size_t i = m.ui.frozen_through;
             i < m.d.current.messages.size(); ++i) {
            mix(m.d.current.messages[i].compute_render_key());
        }
        mix(static_cast<std::uint64_t>(m.d.profile));
        mix_str(m.d.model_id.value);
        mix(m.d.pending_permission ? 1ULL : 0ULL);

        // ── Session / phase.
        mix(static_cast<std::uint64_t>(m.s.phase.index()));
        mix_str(m.s.status);
        // Status expiry: bucket at 100 ms so the gauge’s rolling
        // counter doesn't force a render every microsecond.
        if (m.s.status_until.time_since_epoch().count() != 0) {
            const auto until_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    m.s.status_until.time_since_epoch()).count();
            mix(static_cast<std::uint64_t>(until_ms / 100));
        }
        // Spinner frame bucketed at 10 (its cycle length). Same bucket
        // size the turn-level AgentTimeline cache uses, so a hash
        // advance here corresponds to a new visual.
        if (m.s.active()) {
            mix(static_cast<std::uint64_t>(m.s.spinner.frame_index() % 10));
        }

        // ── Composer / UI.
        mix_str(m.ui.composer.text);
        mix(static_cast<std::uint64_t>(m.ui.composer.cursor));
        mix(m.ui.composer.attachments.size());
        mix(m.ui.composer.queued.size());
        mix(m.ui.composer.expanded ? 1ULL : 0ULL);

        // ── Modal / picker state.
        //
        // CRITICAL: every modal's open/closed state AND its in-modal
        // cursor/query must feed the hash. These pickers are
        // selection-driven — pressing ↑/↓ only mutates an index (or a
        // query string) inside the variant; nothing else in the model
        // moves. If that index isn't hashed, ModelPickerMove produces a
        // model the gate considers visually identical, skip_render fires,
        // and the cursor doesn't repaint until some OTHER hashed axis
        // (caret-blink parity, status text) happens to flip ~265 ms later.
        // Symptom: "press 4-5 times, registers once."
        //
        // variant::index() captures Closed-vs-Open (and login's six
        // sub-states); the OpenAt index / palette query+index capture the
        // cursor movement within an open picker.
        mix(static_cast<std::uint64_t>(m.ui.model_picker.index()));
        mix(static_cast<std::uint64_t>(ui::pick::index_or(m.ui.model_picker)));
        mix(static_cast<std::uint64_t>(m.ui.thread_list.index()));
        mix(static_cast<std::uint64_t>(ui::pick::index_or(m.ui.thread_list)));
        mix(static_cast<std::uint64_t>(m.ui.diff_review.index()));
        if (auto* c = ui::pick::opened(m.ui.diff_review)) {
            mix(static_cast<std::uint64_t>(c->file_index));
            mix(static_cast<std::uint64_t>(c->hunk_index));
        }
        mix(static_cast<std::uint64_t>(m.ui.command_palette.index()));
        if (auto* o = opened(m.ui.command_palette)) {
            mix_str(o->query);
            mix(static_cast<std::uint64_t>(o->index));
        }
        mix(static_cast<std::uint64_t>(m.ui.mention_palette.index()));
        if (auto* o = mention_opened(m.ui.mention_palette)) {
            mix_str(o->query);
            mix(static_cast<std::uint64_t>(o->index));
        }
        mix(static_cast<std::uint64_t>(m.ui.symbol_palette.index()));
        if (auto* o = symbol_palette_opened(m.ui.symbol_palette)) {
            mix_str(o->query);
            mix(static_cast<std::uint64_t>(o->index));
        }
        mix(static_cast<std::uint64_t>(m.ui.todo.open.index()));
        mix(static_cast<std::uint64_t>(m.ui.login.index()));

        // Time-driven animation buckets. Each bucket flip forces a
        // render via hash advance. The bucket size is the FLOOR on
        // how often we'll re-render purely for animation; the actual
        // wake-up cadence is driven by request_animation_frame()
        // deadlines, so a tight bucket here costs nothing when no
        // animation is live.
        //
        // CRITICAL: the hash bucket must be PHASE-LOCKED to whatever
        // animation is actually on screen, otherwise the two beat
        // against each other and frames get skipped — the symptom is a
        // caret that blinks smoothly sometimes and freezes-until-
        // keypress other times. The render gate (skip_render when the
        // hash is unchanged) means: if the hash doesn't advance on the
        // exact frame an animation toggles, the widget's build() never
        // runs, so its request_animation_frame() is never re-armed, the
        // RAF deadline lapses, and the loop sleeps the full idle
        // timeout until the next keypress. So the bucket has to step
        // once per visible animation transition, no faster, no slower.
        //
        // Three regimes:
        //   (a) fine animation live (spinner / streaming caret / welcome
        //       bob / queued-chip pulse): 33 ms (~30 fps) — these advance
        //       every frame, so a fine bucket is correct and necessary.
        //   (b) idle with the composer caret blinking: lock to the blink
        //       HALF-period (265 ms = 530 ms / 2) so every hash step is
        //       exactly one caret toggle. This keeps the RAF loop self-
        //       sustaining (each render re-arms the composer's RAF) and
        //       the blink perfectly regular, at ~4 renders/sec.
        //   (c) nothing animating at all (no caret — e.g. a modal owns
        //       focus): no time bucket, so a settled screen does ZERO
        //       idle renders until an event arrives.
        const auto now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool fine_anim_live =
            m.s.active()                              // spinner / streaming caret
            || m.d.current.messages.empty()           // welcome wordmark bob
            || !m.ui.composer.queued.empty();         // queued-chip pulse
        // The composer caret blinks (maya composer.hpp, U+2588 toggled
        // via style every 265 ms) whenever the composer is idle. maya's
        // own gate is simply `!active`, so match it exactly — if a modal
        // happens to cover the composer the extra ~4 renders/sec are
        // harmless, whereas UNDER-animating would reintroduce the
        // freeze-until-keypress bug. Bias toward animating.
        constexpr std::int64_t kBlinkHalfMs = 265;
        const bool caret_blinking = !m.s.active();
        if (fine_anim_live) {
            mix(static_cast<std::uint64_t>(now_ms / 33));
        } else if (caret_blinking) {
            // Phase-locked: feed the blink PARITY, not a time bucket, so
            // the hash advances on exactly the same boundary maya uses
            // to flip the caret cell. Beat-free by construction.
            mix(static_cast<std::uint64_t>((now_ms / kBlinkHalfMs) & 1));
        }
        // else: nothing animating — contribute no time term at all.

        return k;
    }

    // Optional Program hook (see maya/app/app.hpp — detail::HasNeedsWarmup).
    // Returns true when the next view() result contains a freshly
    // rehydrated frozen scrollback whose cells haven't been captured
    // into maya's component cache yet. The runtime fires a one-shot
    // off-wire warmup_render() before the wire-bound render, which
    // converts the user-visible first frame from O(content) to O(blit)
    // — typically 50–660 ms to <1 ms on tool-heavy thread resume.
    //
    // The flag is set in the reducer (e.g. ThreadLoaded handler in
    // picker.cpp). Maya's loop edge-detects it (rising-edge fires
    // warmup, falling-edge resets the latch), so it's safe to leave
    // the flag set across subsequent reducer steps; we just won't
    // re-warm until it goes false then true again.
    static bool needs_warmup(const Model& m) {
        return m.ui.needs_warmup_render;
    }
};

static_assert(maya::Program<AgenttyApp>);

} // namespace agentty::app
