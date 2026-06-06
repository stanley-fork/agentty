#pragma once
// agentty::acp::AgentServer — the ACP agent that lets agentty run as a
// subprocess Zed (or any ACP client) drives over stdio.
//
// Lifecycle (JSON-RPC methods the client calls on us):
//   initialize          → negotiate protocol version + advertise capabilities
//   authenticate        → no-op success (we authenticate ourselves from
//                          ~/.config/agentty); reports auth-required if creds
//                          are missing
//   session/new         → mint a session id, record cwd, start an empty Thread
//   session/prompt      → run ONE agent turn against the model; stream
//                         session/update notifications as text/tools land;
//                         resolve with a StopReason when the turn settles
//   session/cancel      → (notification) trip the in-flight turn's cancel token
//
// While a turn runs we call BACK to the client:
//   session/update              (notification) — message chunks, tool calls
//   session/request_permission  (request)      — gate WriteFs/Exec tools
//
// The turn loop mirrors the TUI's launch_stream + kick_pending_tools
// (src/runtime/app/cmd_factory.cpp) but without maya: same provider, same
// tools::registry, same permission policy, same wire-message shaping.
//
// The provider is type-erased behind a std::function so the turn loop is
// testable against a scripted in-memory provider, exactly like the TUI.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/acp/jsonrpc.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/domain/conversation.hpp"
#include "agentty/domain/profile.hpp"
#include "agentty/io/http.hpp"
#include "agentty/provider/provider.hpp"

namespace agentty::acp {

// The provider call, type-erased: (Request, EventSink) → void. Matches
// provider::Provider::stream. Lets the server hold any provider without
// templating the whole class.
using StreamFn =
    std::function<void(provider::Request, provider::EventSink)>;

// Per-session state: one agentty Thread + the workspace cwd the client
// opened the session against, plus the in-flight turn's cancel handle.
struct Session {
    std::string id;
    std::string cwd;
    Thread      thread;
    std::shared_ptr<http::CancelToken> cancel;
};

class AgentServer {
public:
    // `stream` is the provider entrypoint (bind AnthropicProvider::stream or
    // a test double). `auth` is the resolved wire credential; may be empty,
    // in which case prompts report authentication-required. `model_id` is the
    // default model for new sessions.
    AgentServer(rpc::Peer&        peer,
                StreamFn          stream,
                auth::AuthHeader  auth,
                std::string       model_id,
                Profile           profile = Profile::Ask);

    // Install handlers and block on peer.run() until the client disconnects.
    // Returns a process exit code.
    int serve();

private:
    // ── JSON-RPC method handlers ─────────────────────────────────────────
    rpc::Outcome handle_request(const std::string& method,
                                const nlohmann::json& params,
                                const nlohmann::json& id);
    void         handle_notification(const std::string& method,
                                     const nlohmann::json& params);

    nlohmann::json on_initialize(const nlohmann::json& params);
    nlohmann::json on_new_session(const nlohmann::json& params);
    // session/load: restore a persisted Thread from disk, replay its history
    // as session/update notifications, then resolve. Returns the load result
    // (null body). Throws on unknown / unreadable session id.
    nlohmann::json on_load_session(const nlohmann::json& params);
    void           on_prompt(const nlohmann::json& id, const nlohmann::json& params);
    void           on_cancel(const nlohmann::json& params);

    // ── The headless turn loop ───────────────────────────────────────────
    // Runs to completion on a worker thread. Streams session/update
    // notifications and resolves the deferred session/prompt response with a
    // stopReason. `session_id` keys the session under session_mtx_.
    void run_turn(nlohmann::json prompt_id, std::string session_id);

    // Drive ONE model completion (one SSE stream): build the wire request,
    // call stream_, translate Msg events to ACP session/update. Returns the
    // anthropic StopReason of the completion and appends the assistant
    // Message (with any tool_calls) to the session thread. `out_cancelled`
    // is set if the turn was cancelled mid-stream.
    StopReason stream_completion(Session& sess, bool& out_cancelled,
                                 std::string& out_error);

    // Execute every pending tool call on the last assistant message,
    // requesting permission where the policy demands it. Appends the
    // tool_result-bearing User message. Returns false if the turn was
    // cancelled while awaiting permission.
    bool run_tools(Session& sess, bool& out_cancelled);

    // ── Helpers ──────────────────────────────────────────────────────────
    void send_update(const std::string& session_id, nlohmann::json update);
    // session/request_permission round-trip. Returns true if allowed.
    bool ask_permission(const std::string& session_id, const ToolUse& tc);

    Session* find_session(const std::string& id);

    // Persist a session's Thread to the on-disk store (same format the TUI
    // uses, so ACP sessions show up in the thread picker and survive a
    // subprocess restart). Called after every turn and on session creation.
    void persist(const Session& sess);

    // Replay a thread's full conversation history to the client as
    // session/update notifications (user_message_chunk / agent_message_chunk
    // + tool_call cards), per the session/load contract.
    void replay_history(const std::string& session_id, const Thread& thread);

    // Wire tool list, built once from the static tools::registry(). The
    // registry never changes at runtime, so we snapshot it on first use
    // instead of rebuilding the vector every completion.
    const std::vector<provider::ToolSpec>& wire_tools();

    rpc::Peer&       peer_;
    StreamFn         stream_;
    auth::AuthHeader auth_;
    std::string      model_id_;
    Profile          profile_;

    std::once_flag                  tools_once_;
    std::vector<provider::ToolSpec> wire_tools_;

    std::mutex                                   session_mtx_;
    std::unordered_map<std::string, Session>     sessions_;
    std::uint64_t                                next_tool_uid_ = 1;
};

} // namespace agentty::acp
