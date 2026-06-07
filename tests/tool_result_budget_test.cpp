// tool_result_budget_test — wire-only byte-budget truncation for tool
// results. A single oversized tool output (500 KiB grep, dump of a big
// file) must NOT ship verbatim on the wire; it replays on every turn
// otherwise and burns tokens long before auto-compaction fires.
//
// Drives the REAL wire serializer (messages_json_string) over an
// Assistant turn carrying a Done tool_call with a giant output, and
// asserts:
//   1. the serialized tool_result content is bounded (<= budget + slack)
//   2. the elision marker is present
//   3. BOTH head and tail bytes survive (Zed keeps both ends)
//   4. a SMALL result is byte-identical (no marker, no truncation)
//   5. the cut lands on a UTF-8 boundary (no mojibake / no invalid json)

#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/domain/conversation.hpp"
#include "agentty/provider/anthropic/transport.hpp"

using agentty::Message;
using agentty::Role;
using agentty::Thread;
using agentty::ThreadId;
using agentty::ToolCallId;
using agentty::ToolName;
using agentty::ToolUse;

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_fails; }
    else     { std::fprintf(stderr, "ok:   %s\n", what); }
}

// Build a one-turn thread: User prompt + Assistant turn with a single
// Done tool_call carrying `output` as its result.
Thread make_thread_with_tool_output(const std::string& output) {
    Message user;
    user.role = Role::User;
    user.text = "do a thing";

    ToolUse tc;
    tc.id   = ToolCallId{"toolu_test_1"};
    tc.name = ToolName{"grep"};
    tc.status = ToolUse::Done{
        std::chrono::steady_clock::now(),
        std::chrono::steady_clock::now(),
        output,
    };

    Message asst;
    asst.role = Role::Assistant;
    asst.text = "running grep";
    asst.tool_calls.push_back(std::move(tc));

    return Thread{ThreadId{"t"}, "", {user, asst}, {}, {}};
}

// Extract the tool_result content string for tool_use_id from the wire.
std::string tool_result_content(const std::string& wire, const char* tool_use_id) {
    auto j = nlohmann::json::parse(wire);
    for (const auto& msg : j) {
        if (msg.value("role", "") != "user") continue;
        for (const auto& block : msg.value("content", nlohmann::json::array())) {
            if (block.value("type", "") != "tool_result") continue;
            if (block.value("tool_use_id", "") != tool_use_id) continue;
            return block.value("content", "");
        }
    }
    return {};
}

} // namespace

int main() {
    namespace ap = agentty::provider::anthropic;

    // ── 1. Small result: byte-identical passthrough, no marker ──
    {
        const std::string small = "matched 3 lines\nfoo\nbar\nbaz\n";
        Thread t = make_thread_with_tool_output(small);
        std::string wire = ap::messages_json_string(t);
        std::string content = tool_result_content(wire, "toolu_test_1");
        check(content == small, "small result ships verbatim");
        check(content.find("bytes elided") == std::string::npos,
              "small result has no elision marker");
    }

    // ── 2. Oversized result: bounded, marker present, head+tail kept ──
    {
        // 500 KiB: a HEAD_MARKER..filler..TAIL_MARKER sandwich so we can
        // assert both ends survived the cut.
        std::string big;
        big.reserve(512 * 1024);
        big += "HEAD_SENTINEL_AAAA\n";
        big.append(500 * 1024, 'x');
        big += "\nTAIL_SENTINEL_ZZZZ";

        Thread t = make_thread_with_tool_output(big);
        std::string wire = ap::messages_json_string(t);
        std::string content = tool_result_content(wire, "toolu_test_1");

        // Budget is 64 KiB; serialized content (after JSON unescaping)
        // must be far smaller than the 500 KiB input. Allow generous
        // slack for marker + boundary rounding.
        check(content.size() < 80 * 1024,
              "oversized result is bounded near the 64 KiB budget");
        check(content.size() < big.size(),
              "oversized result is smaller than the raw output");
        check(content.find("bytes elided") != std::string::npos,
              "oversized result carries the elision marker");
        check(content.find("HEAD_SENTINEL_AAAA") != std::string::npos,
              "head bytes survive the cut");
        check(content.find("TAIL_SENTINEL_ZZZZ") != std::string::npos,
              "tail bytes survive the cut");
    }

    // ── 3. Multibyte boundary: cut must not split a UTF-8 sequence ──
    {
        // Fill with a 3-byte code point (U+4E2D, CJK) so a naive byte cut
        // would land mid-sequence. The wire must still parse as valid
        // JSON (it would throw on invalid UTF-8 / split escape).
        std::string mb;
        const std::string cp = "\xE4\xB8\xAD"; // 中
        mb.reserve(200 * 1024);
        for (int i = 0; i < 70 * 1024; ++i) mb += cp;

        Thread t = make_thread_with_tool_output(mb);
        bool parsed_ok = true;
        std::string content;
        try {
            std::string wire = ap::messages_json_string(t);
            content = tool_result_content(wire, "toolu_test_1");
        } catch (...) {
            parsed_ok = false;
        }
        check(parsed_ok, "multibyte oversized result yields valid JSON");
        check(!content.empty(), "multibyte result content non-empty");
    }

    if (g_fails == 0) std::fprintf(stderr, "\nALL PASS\n");
    else              std::fprintf(stderr, "\n%d FAILURE(S)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
