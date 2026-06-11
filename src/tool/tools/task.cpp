// task — subagent dispatch tool.
//
// Spawns an isolated agent loop: a fresh single-message Thread seeded
// with the caller's prompt, streamed against the provider, with the
// model's tool calls executed locally, looping until the subagent
// finishes (end_turn) or hits the turn budget. Returns ONE condensed
// text result to the parent agent — the subagent's full transcript is
// NOT surfaced, keeping the parent's context clean. Mirrors Claude
// Code's `Task` / Zed's `spawn_agent`.
//
// Isolation properties:
//   • Own Thread / context — parent transcript is never sent.
//   • Own tool budget (kMaxTurns completions).
//   • Depth-capped (kMaxDepth) so subagents-spawning-subagents can't
//     fork-bomb.
//   • Synchronous: runs entirely inside execute() on the parent tool's
//     worker thread (run_tool is task_isolated), so no Elm async
//     machinery and no per-event reducer cost.

#include "agentty/tool/spec.hpp"
#include "agentty/tool/subagent.hpp"
#include "agentty/tool/tool.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/registry.hpp"  // tools::progress::emit
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/tool_args.hpp"

#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/provider.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::tools {

using json = nlohmann::json;

namespace {

struct TaskArgs {
    std::string prompt;
    std::string display_description;
};

std::expected<TaskArgs, ToolError> parse_task_args(const json& j) {
    util::ArgReader ar(j);
    TaskArgs out;
    out.prompt = ar.str("prompt", "");
    out.display_description = ar.str("display_description", "");
    if (out.prompt.empty())
        return std::unexpected(ToolError::invalid_args(
            "task requires a non-empty `prompt` describing the subagent's goal"));
    return out;
}

// System prompt for the subagent. Deliberately terse + outcome-focused:
// the subagent exists to do ONE delegated job and report back, not to
// chat. It reuses the same tool catalog as the parent.
std::string subagent_system_prompt() {
    std::string base = provider::anthropic::default_system_prompt();
    base +=
        "\n\n<subagent>\n"
        "You are a SUBAGENT spawned to complete one delegated task in "
        "isolation. You do NOT see the parent conversation. Work "
        "autonomously: use tools to investigate and act, then finish "
        "with a single concise report of what you found or did. Do not "
        "ask the parent questions — you cannot receive answers. When the "
        "task is complete, stop calling tools and write your final "
        "report as plain text. Keep the report tight: the parent only "
        "gets your final message, not your transcript.\n"
        "</subagent>";
    return base;
}

// A short one-line summary of a tool call for the live progress log:
// the tool name plus its most identifying argument (path / pattern /
// command), so the parent card reads like a running activity feed.
std::string summarize_call(const ToolUse& tc) {
    std::string s = tc.name.value;
    if (tc.args.is_object()) {
        for (const char* key : {"path", "file_path", "pattern", "command",
                                "url", "query", "symbol", "prompt"}) {
            auto it = tc.args.find(key);
            if (it != tc.args.end() && it->is_string()) {
                std::string v = it->get<std::string>();
                if (v.size() > 80) { v.resize(77); v += "..."; }
                // collapse newlines so the feed stays one-line-per-call
                for (auto& ch : v) if (ch == '\n' || ch == '\r') ch = ' ';
                s += "  ";
                s += v;
                break;
            }
        }
    }
    return s;
}

// Drive ONE subagent completion synchronously. Appends the assistant
// Message (text + tool_calls) to `thread`. Returns the stop reason.
// Reassembles streamed Msg events into the assistant Message. `log`
// accumulates a human-readable activity feed that is pushed to the
// parent's tool card via progress::emit as the subagent works — so the
// running card shows what the subagent is doing instead of sitting
// blank until the final report lands.
StopReason run_one_completion(Thread& thread, const subagent::Config& cfg,
                              std::string& log, std::string& err_out) {
    namespace ap = provider::anthropic;
    ap::Request req;
    req.model         = cfg.model;
    req.system_prompt = subagent_system_prompt();
    req.auth          = cfg.auth;
    req.max_tokens    = provider::kSafeMaxTokens;
    req.messages      = thread.messages;
    for (const auto& t : tools::registry()) {
        // The subagent does NOT get the `task` tool itself beyond the
        // depth cap; the cap (checked in run_task) is the real guard,
        // but we still expose the full catalog so a depth-1 subagent
        // can delegate once more if genuinely needed.
        req.tools.push_back({t.name.value, t.description, t.input_schema,
                             t.eager_input_streaming});
    }

    Message asst;
    asst.role = Role::Assistant;

    StopReason stop = StopReason::Unspecified;
    std::string cur_tool_json;   // accumulates input_json_delta for the open tool

    // Emit a snapshot of the activity log plus whatever the subagent has
    // streamed so far this turn. Cheap (no-op if no parent sink), so we
    // can call it on every text delta for a live typing effect.
    auto pump = [&] {
        std::string snap = log;
        if (!asst.text.empty()) {
            snap += "\n  \xe2\x96\xb8 ";  // ▸
            snap += asst.text;
        }
        progress::emit(snap);
    };

    ap::run_stream_sync(std::move(req),
        [&](Msg m) {
            // Msg is a TWO-LEVEL variant: the top level is the domain
            // (ComposerMsg / StreamMsg / ToolMsg / ...) and each domain is
            // itself a variant of leaf events. The transport only ever
            // emits StreamMsg leaves, so unwrap the domain first, then
            // visit the inner StreamMsg to reach StreamTextDelta etc. The
            // previous single std::visit matched T against StreamTextDelta
            // directly — but T was msg::StreamMsg (the nested variant), so
            // NO arm ever fired and every delta was silently dropped
            // (subagent always "finished with no text report").
            auto* sm = std::get_if<msg::StreamMsg>(&m);
            if (!sm) return;  // non-stream domain can't arrive on this path
            std::visit([&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::same_as<T, StreamTextDelta>) {
                    asst.text += e.text;
                    pump();
                } else if constexpr (std::same_as<T, StreamToolUseStart>) {
                    ToolUse tc;
                    tc.id     = e.id;
                    tc.name   = e.name;
                    tc.status = ToolUse::Pending{std::chrono::steady_clock::now()};
                    asst.tool_calls.push_back(std::move(tc));
                    cur_tool_json.clear();
                } else if constexpr (std::same_as<T, StreamToolUseDelta>) {
                    cur_tool_json += e.partial_json;
                } else if constexpr (std::same_as<T, StreamToolUseEnd>) {
                    if (!asst.tool_calls.empty() && !cur_tool_json.empty()) {
                        try {
                            asst.tool_calls.back().args = json::parse(cur_tool_json);
                        } catch (...) {
                            // Leave args null; run_task marks it failed.
                        }
                    }
                    cur_tool_json.clear();
                    if (!asst.tool_calls.empty()) {
                        log += "\n  \xe2\x9a\x99 ";  // ⚙
                        log += summarize_call(asst.tool_calls.back());
                        pump();
                    }
                } else if constexpr (std::same_as<T, StreamFinished>) {
                    stop = e.stop_reason;
                } else if constexpr (std::same_as<T, StreamError>) {
                    // Surface the failure instead of swallowing it — an
                    // auth/400/rate-limit error inside the subagent thread
                    // otherwise looks like "finished with no text report".
                    err_out = e.message;
                    log += "\n  \xe2\x9a\xa0 error: " + e.message;  // ⚠
                    pump();
                }
                // StreamStarted / StreamUsage / heartbeat: ignored.
            }, *sm);
        });

    thread.messages.push_back(std::move(asst));
    return stop;
}

ExecResult run_task(const TaskArgs& a) {
    auto cfg = subagent::current();
    if (!cfg.installed || cfg.model.empty() || auth::is_empty(cfg.auth)) {
        return std::unexpected(ToolError::invalid_args(
            "subagents are unavailable in this context (no model/auth wired)"));
    }
    if (subagent::current_depth() >= subagent::kMaxDepth) {
        return std::unexpected(ToolError::invalid_args(
            "subagent depth limit reached — a subagent cannot spawn further "
            "subagents at this nesting level"));
    }

    subagent::push_depth();
    struct DepthGuard {
        ~DepthGuard() { subagent::pop_depth(); }
    } depth_guard;

    // Seed the subagent thread with the delegated prompt.
    Thread thread;
    {
        Message user;
        user.role = Role::User;
        user.text = a.prompt;
        thread.messages.push_back(std::move(user));
    }

    int turns = 0;
    // Human-readable activity feed streamed to the parent's tool card and
    // appended to the final report so even a no-text-report subagent shows
    // what it actually did.
    std::string log;
    std::string last_error;  // last StreamError, if any
    while (turns < subagent::kMaxTurns) {
        ++turns;
        log += (turns == 1 ? "" : "\n");
        log += "\xe2\x80\xa2 turn " + std::to_string(turns);  // •
        progress::emit(log);
        std::string err;
        StopReason stop = run_one_completion(thread, cfg, log, err);
        if (!err.empty()) last_error = err;

        Message& asst = thread.messages.back();

        // Execute any tool calls the subagent emitted; append a User turn
        // carrying the results so the next completion sees them.
        bool ran_a_tool = false;
        if (!asst.tool_calls.empty()) {
            const auto now = std::chrono::steady_clock::now();
            for (auto& tc : asst.tool_calls) {
                if (tc.args.is_null()) {
                    tc.status = ToolUse::Failed{now, now,
                        "subagent tool args failed to parse"};
                    log += "\n    \xe2\x9c\x97 " + tc.name.value + ": bad args";
                    progress::emit(log);
                    continue;
                }
                ran_a_tool = true;
                auto res = tool::DynamicDispatch::execute(tc.name.value, tc.args);
                if (res) {
                    tc.status = ToolUse::Done{now, now, std::move(res->text)};
                    log += "\n    \xe2\x9c\x93 " + tc.name.value;  // ✓
                } else {
                    tc.status = ToolUse::Failed{now, now, res.error().render()};
                    log += "\n    \xe2\x9c\x97 " + tc.name.value + ": "  // ✗
                         + res.error().render();
                }
                progress::emit(log);
            }
            // The transport synthesises the tool_result User turn from the
            // assistant's terminal tool_calls, so we don't push one here —
            // appending the next placeholder is enough to continue the loop.
        }

        // Stop when the model ended its turn without (further) tool use.
        if (stop != StopReason::ToolUse && !ran_a_tool) break;
        // Defensive: if the model claimed tool_use but emitted nothing we
        // could run, stop rather than spin.
        if (stop == StopReason::ToolUse && !ran_a_tool) break;
        // A hard stream error (auth/400/etc.) won't fix itself by looping.
        if (!err.empty() && !ran_a_tool) break;
    }

    // Harvest the final report: the last assistant message's text.
    std::string report;
    for (auto it = thread.messages.rbegin(); it != thread.messages.rend(); ++it) {
        if (it->role == Role::Assistant && !it->text.empty()) {
            report = it->text;
            break;
        }
    }
    if (report.empty()) {
        // No final text report. Surface the real reason: a stream error
        // (auth/400/rate-limit) is the usual culprit and was previously
        // swallowed, leaving a bare "finished without a text report".
        // Fall back to the activity feed so the parent sees what happened.
        std::string why;
        if (!last_error.empty())
            why = "[subagent failed: " + last_error + "]";
        else if (turns >= subagent::kMaxTurns)
            why = "[subagent hit its turn budget without producing a final report]";
        else
            why = "[subagent finished without a text report]";
        report = log.empty() ? why : (why + "\n\nActivity:\n" + log);
    }

    std::ostringstream out;
    out << "Subagent report (" << turns << " turn"
        << (turns == 1 ? "" : "s") << "):\n\n" << report;
    return ToolOutput{out.str(), std::nullopt};
}

} // namespace

ToolDef tool_task() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"task">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Spawn an autonomous subagent to complete a self-contained task "
        "in isolation (own context, own tool budget). Use for focused "
        "investigations or multi-step jobs you want handled end-to-end "
        "without cluttering this conversation — e.g. \"find every call "
        "site of X and summarize them\", \"reproduce and diagnose this "
        "failing test\". The subagent runs to completion and returns a "
        "single condensed report; you do NOT see its intermediate steps. "
        "It cannot ask you questions, so give a complete, unambiguous "
        "prompt.";
    t.input_schema = json{
        {"type", "object"},
        {"required", {"prompt"}},
        {"properties", {
            {"prompt", {{"type", "string"},
                {"description", "Complete, self-contained description of the "
                                "task for the subagent to accomplish."}}},
            {"display_description", {{"type", "string"},
                {"description", "One-line summary shown in the UI. Optional."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<TaskArgs>(parse_task_args, run_task);
    return t;
}

} // namespace agentty::tools
