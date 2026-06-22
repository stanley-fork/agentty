#include "agentty/tool/spec.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/tool/util/fuzzy_match.hpp"
#include "agentty/tool/util/tool_args.hpp"
#include "agentty/diff/diff.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// ── Char-level minimal splice ───────────────────────────────────────────
//
// When fuzzy_find matches a region [pos, pos+len) and we have a
// replacement `repl`, the naive splice swaps the WHOLE region for
// `repl`. But the matched region and `repl` usually share a large
// common prefix and suffix (the model edits one line in a 30-line
// block; or fuzzy-match tolerated trailing whitespace that's actually
// identical). Swapping the whole region forces those identical bytes to
// be marked deleted+re-inserted, which (a) bloats the unified diff the
// reviewer reads and (b) needlessly perturbs trailing context.
//
// `minimal_splice` trims the common prefix and suffix between the
// matched slice and `repl` so only the genuinely-changed middle run is
// substituted. The unchanged head/tail bytes survive byte-identically
// into the new buffer. This is Zed's StreamingDiff keep/insert/delete
// idea, applied once at splice time rather than incrementally.
//
// We snap the trim boundaries to UTF-8 code-point edges so we never
// split a multibyte sequence, and (cosmetically) back off to the
// nearest preceding newline/whitespace so the resulting hunk lands on
// clean token boundaries rather than mid-identifier. Returns the
// rewritten full buffer. Falls back to the whole-region swap if the
// two sides share nothing.
inline bool is_utf8_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

std::string minimal_splice(const std::string& buf,
                           std::size_t region_pos,
                           std::size_t region_len,
                           std::string_view repl) {
    std::string_view matched{buf.data() + region_pos, region_len};

    // Common prefix length (bytes).
    std::size_t pre = 0;
    const std::size_t maxpre = std::min(matched.size(), repl.size());
    while (pre < maxpre && matched[pre] == repl[pre]) ++pre;
    // Snap back off a partial UTF-8 sequence: never cut inside a
    // multibyte code point.
    while (pre > 0 && is_utf8_cont(static_cast<unsigned char>(
               pre < matched.size() ? matched[pre] : '\0')))
        --pre;

    // Common suffix length (bytes), not overlapping the prefix.
    std::size_t suf = 0;
    const std::size_t maxsuf =
        std::min(matched.size() - pre, repl.size() - pre);
    while (suf < maxsuf
           && matched[matched.size() - 1 - suf] == repl[repl.size() - 1 - suf])
        ++suf;
    // Snap the suffix boundary forward off a UTF-8 continuation byte so
    // the kept-suffix starts on a code-point edge.
    while (suf > 0
           && is_utf8_cont(static_cast<unsigned char>(
                  matched[matched.size() - suf])))
        --suf;

    // If prefix+suffix consume the entire region AND the entire repl,
    // the two sides are byte-identical post-trim — nothing to splice.
    // (Caller already filtered pure no-ops, but fuzzy indent fix-ups can
    // land here; treat as no change.)
    if (pre + suf >= matched.size() && pre + suf >= repl.size())
        return buf;

    // Nothing shared → whole-region swap (the original behaviour).
    if (pre == 0 && suf == 0) {
        std::string out;
        out.reserve(buf.size() - region_len + repl.size());
        out.append(buf, 0, region_pos);
        out.append(repl);
        out.append(buf, region_pos + region_len, std::string::npos);
        return out;
    }

    // Splice only the changed middle: keep buf[0 .. region_pos+pre),
    // replace matched-middle with repl-middle, keep the rest.
    std::string_view repl_mid{repl.data() + pre, repl.size() - pre - suf};
    const std::size_t cut_begin = region_pos + pre;
    const std::size_t cut_end   = region_pos + region_len - suf;
    std::string out;
    out.reserve(buf.size() - (cut_end - cut_begin) + repl_mid.size());
    out.append(buf, 0, cut_begin);
    out.append(repl_mid);
    out.append(buf, cut_end, std::string::npos);
    return out;
}

struct OneEdit {
    std::string old_text;
    std::string new_text;
    bool        replace_all = false;
    // 0 = caller didn't pin a count (use replace_all flag instead).
    // N>0 = Anthropic-spec contract: edit MUST match exactly N times or fail.
    // Maps to the official text_editor `expected_replacements` field; we
    // treat N>=2 the same as replace_all=true for execution purposes, but
    // we ALSO verify the count matches and fail with a precise mismatch
    // error if it doesn't. Safer than `replace_all: true` because a stale
    // expectation surfaces as an error instead of a silent over-replace.
    int         expected_replacements = 0;
    // Optional 1-based line number hint for breaking ambiguity. When the
    // fuzzy matcher finds multiple equally-good candidate regions, the
    // one whose start line is closest to `line_hint` (within ~200 lines)
    // wins. UINT32_MAX = no hint. Mirrors Zed's `line` field.
    std::uint32_t line_hint = std::numeric_limits<std::uint32_t>::max();
};

struct EditArgs {
    util::WorkspacePath   path;
    std::vector<OneEdit>  edits;
    std::string           display_description;  // one-line UI summary; optional
};

// Accepts three shapes, in preference order:
//   1. `edits: [{old_text, new_text, replace_all?}, ...]`   (new canonical)
//   2. `old_text` / `new_text` at top level    (Zed's legacy single-edit)
//   3. `old_string` / `new_string` at top level (agentty's original schema)
// Missing / wrong-typed fields are tolerated where recoverable — we only
// return an error when there is genuinely nothing to do.
std::expected<EditArgs, ToolError> parse_edit_args(const json& j) {
    util::ArgReader ar(j);
    if (!ar.is_object())
        return std::unexpected(ToolError::invalid_args("args must be an object"));

    auto path_opt = ar.require_str("path");
    if (!path_opt)
        return std::unexpected(ToolError::invalid_args("path required"));
    auto wp = util::make_workspace_path_checked(*path_opt, "edit");
    if (!wp) return std::unexpected(std::move(wp.error()));

    std::string desc = ar.str("display_description", "");
    std::vector<OneEdit> edits;

    // Shape 1: edits array.
    if (auto raw = ar.raw("edits"); raw && raw->is_array()) {
        edits.reserve(raw->size());
        int idx = 0;
        for (const auto& e : *raw) {
            ++idx;
            if (!e.is_object())
                return std::unexpected(ToolError::invalid_args(
                    std::format("edits[{}]: expected object", idx - 1)));
            util::ArgReader sub(e);
            auto old_opt = sub.require_str("old_text");
            // Accept old_string too.
            if (!old_opt) old_opt = sub.require_str("old_string");
            if (!old_opt)
                return std::unexpected(ToolError::invalid_args(
                    std::format("edits[{}]: old_text required", idx - 1)));
            std::string new_text = sub.str("new_text", "");
            if (new_text.empty() && sub.has("new_string"))
                new_text = sub.str("new_string", "");
            // Accept `expected_replacements` (Anthropic spec) or the shorter
            // `count` alias. Default 0 means "caller didn't pin", which
            // falls back to the legacy replace_all bool.
            int expected = sub.integer("expected_replacements", 0);
            if (expected == 0) expected = sub.integer("count", 0);
            bool replace_all = sub.boolean("replace_all", false);
            if (expected >= 2) replace_all = true;
            // Optional line anchor (1-based, like the user sees in their
            // editor). Aliases: `line`, `line_hint`, `at_line`. Stored
            // 0-based internally to match fuzzy_find's coordinate system.
            std::uint32_t line_hint = std::numeric_limits<std::uint32_t>::max();
            int ln = sub.integer("line", 0);
            if (ln <= 0) ln = sub.integer("line_hint", 0);
            if (ln <= 0) ln = sub.integer("at_line", 0);
            if (ln > 0) line_hint = static_cast<std::uint32_t>(ln - 1);
            edits.push_back(OneEdit{
                std::move(*old_opt),
                std::move(new_text),
                replace_all,
                expected,
                line_hint,
            });
        }
    } else {
        // Shape 2/3: single edit at top level (old_text or old_string).
        auto old_opt = ar.require_str("old_string");
        if (!old_opt) old_opt = ar.require_str("old_text");
        if (!old_opt)
            return std::unexpected(ToolError::invalid_args(
                "no edits provided — pass either `edits: [...]` or "
                "`old_string`/`new_string` at top level"));
        std::string new_s = ar.str("new_string", "");
        if (new_s.empty() && ar.has("new_text"))
            new_s = ar.str("new_text", "");
        int expected = ar.integer("expected_replacements", 0);
        if (expected == 0) expected = ar.integer("count", 0);
        bool replace_all = ar.boolean("replace_all", false);
        if (expected >= 2) replace_all = true;
        std::uint32_t line_hint = std::numeric_limits<std::uint32_t>::max();
        int ln = ar.integer("line", 0);
        if (ln <= 0) ln = ar.integer("line_hint", 0);
        if (ln <= 0) ln = ar.integer("at_line", 0);
        if (ln > 0) line_hint = static_cast<std::uint32_t>(ln - 1);
        edits.push_back(OneEdit{
            std::move(*old_opt), std::move(new_s),
            replace_all, expected, line_hint,
        });
    }

    if (edits.empty())
        return std::unexpected(ToolError::invalid_args(
            "edits array is empty — nothing to change"));

    // Per-edit sanity: empty old_text is never legal. Identical old==new
    // is silently skipped at apply time (see run_edit); failing the whole
    // batch over a no-op is hostile when the other edits are valid.
    for (std::size_t i = 0; i < edits.size(); ++i) {
        const auto& e = edits[i];
        if (e.old_text.empty())
            return std::unexpected(ToolError::invalid_args(
                std::format("edits[{}]: old_text cannot be empty", i)));
    }

    return EditArgs{
        std::move(*wp),
        std::move(edits),
        std::move(desc),
    };
}

// Render the line number a byte offset falls on (1-based), for error
// messages. O(offset) but only invoked on the failure path.
int line_of_offset(std::string_view s, std::size_t off) noexcept {
    int line = 1;
    auto cap = std::min(off, s.size());
    for (std::size_t i = 0; i < cap; ++i) if (s[i] == '\n') ++line;
    return line;
}

// First few line numbers where `needle` appears in `buf`, capped. Used to
// turn "appears N times" into "appears at lines 12, 47, 113…" so the
// model knows which one to disambiguate.
std::vector<int> hit_lines(std::string_view buf, std::string_view needle,
                           std::size_t cap = 5) {
    std::vector<int> out;
    if (needle.empty()) return out;
    std::size_t p = 0;
    while (out.size() < cap) {
        auto pos = buf.find(needle, p);
        if (pos == std::string_view::npos) break;
        out.push_back(line_of_offset(buf, pos));
        p = pos + needle.size();
    }
    return out;
}

std::string join_ints(const std::vector<int>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ", ";
        out += std::to_string(v[i]);
    }
    return out;
}

// Find the line in `buf` most similar to the first non-blank line of
// `needle`, returning its 1-based line number, or 0 if no reasonable
// candidate exists. Used to give the model an anchor when fuzzy_find
// strikes out entirely. Cheap: O(file_lines * first_needle_line_len).
int closest_line_hint(std::string_view buf, std::string_view needle) noexcept {
    // Extract the first non-blank trimmed needle line.
    std::string_view probe;
    std::size_t i = 0;
    while (i < needle.size()) {
        std::size_t s = i;
        while (i < needle.size() && needle[i] != '\n') ++i;
        std::size_t e = i;
        if (i < needle.size()) ++i;
        // trim ws
        while (s < e && (needle[s] == ' ' || needle[s] == '\t' || needle[s] == '\r')) ++s;
        while (e > s && (needle[e-1] == ' ' || needle[e-1] == '\t' || needle[e-1] == '\r')) --e;
        if (e > s) { probe = needle.substr(s, e - s); break; }
    }
    if (probe.size() < 4) return 0;   // too short to anchor reliably
    // Walk the file by line; score = longest common substring length with probe.
    int best_ln = 0;
    std::size_t best_score = 0;
    std::size_t pos = 0;
    int ln = 1;
    while (pos < buf.size()) {
        std::size_t le = pos;
        while (le < buf.size() && buf[le] != '\n') ++le;
        std::string_view line = buf.substr(pos, le - pos);
        // Trim line.
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
            line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.remove_suffix(1);
        // Score = length of longest probe prefix that appears anywhere in the line.
        // Cheap proxy: walk down probe size until it fits.
        std::size_t score = 0;
        if (!line.empty()) {
            for (std::size_t plen = probe.size(); plen >= 4; --plen) {
                if (line.find(probe.substr(0, plen)) != std::string_view::npos) {
                    score = plen; break;
                }
            }
        }
        if (score > best_score) { best_score = score; best_ln = ln; }
        pos = (le < buf.size()) ? le + 1 : le;
        ++ln;
    }
    // Require half of the probe to overlap before suggesting; otherwise
    // the hint is noise.
    return (best_score * 2 >= probe.size()) ? best_ln : 0;
}

// Render a small window of lines centred on `line_1based`, prefixed with
// their line numbers. Used by no-match error messages so the model has
// the EXACT bytes around the closest hit to copy without re-reading.
// Pure text — no ANSI — lands cleanly inside a tool_result envelope.
std::string render_context_window(std::string_view buf,
                                  int line_1based,
                                  int radius = 2) {
    if (line_1based <= 0) return {};
    int lo = std::max(1, line_1based - radius);
    int hi = line_1based + radius;
    std::string out;
    int ln = 1;
    std::size_t pos = 0;
    while (pos < buf.size() && ln <= hi) {
        std::size_t le = pos;
        while (le < buf.size() && buf[le] != '\n') ++le;
        if (ln >= lo) {
            char prefix[16];
            std::snprintf(prefix, sizeof(prefix), "%4d %s ",
                          ln, ln == line_1based ? ">" : " ");
            out += prefix;
            out.append(buf.data() + pos, le - pos);
            out.push_back('\n');
        }
        pos = (le < buf.size()) ? le + 1 : le;
        ++ln;
    }
    return out;
}

// Apply a single edit to `buf`. Returns number of replacements; sets `err`
// on terminal failure (ambiguous match, not found). When the edit's
// `new_text` already appears in `buf` where `old_text` would have matched
// (the model is re-applying a change that already landed), returns -1 to
// signal "idempotent no-op" — the caller treats this as a soft success
// rather than a hard error.
constexpr int kIdempotentNoOp = -1;

int apply_one(std::string& buf, const OneEdit& e,
              const std::string& path_str, std::string& err) {
    auto replace_exact = [&](std::string_view needle,
                             std::string_view replacement) -> int {
        std::string out;
        out.reserve(buf.size());
        std::size_t cursor = 0;
        int n = 0;
        for (;;) {
            auto pos = buf.find(needle, cursor);
            if (pos == std::string::npos) break;
            out.append(buf, cursor, pos - cursor);
            out.append(replacement);
            cursor = pos + needle.size();
            ++n;
        }
        if (n == 0) return 0;
        out.append(buf, cursor, std::string::npos);
        buf = std::move(out);
        return n;
    };

    // CRLF normalisation helpers used by multiple strategies.
    auto without_cr = [](std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) if (c != '\r') out.push_back(c);
        return out;
    };
    auto with_crlf = [](std::string_view s) {
        std::string out;
        out.reserve(s.size() + s.size() / 40);
        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c == '\n' && (i == 0 || s[i-1] != '\r'))
                out.push_back('\r');
            out.push_back(c);
        }
        return out;
    };

    // ── Idempotency check (runs FIRST, covers both branches) ──────────
    // The model often re-applies an edit that already landed (it re-read
    // the file, saw the new content, is "confirming"). old_text is gone
    // — replaced by new_text — so fuzzy_find returns no-match and we
    // surface a confusing error. Detect this up front: if `new_text`
    // is non-empty and substantive AND already in the buffer AND
    // `old_text` is absent, treat as idempotent no-op. The 8-byte size
    // gate filters out tiny new_texts whose accidental presence in the
    // buffer would false-positive.
    if (!e.new_text.empty()
        && e.new_text.size() >= 8
        && e.new_text != e.old_text
        && buf.find(e.new_text) != std::string::npos
        && buf.find(e.old_text) == std::string::npos)
    {
        err.clear();
        return kIdempotentNoOp;
    }

    if (e.replace_all) {
        int n = replace_exact(e.old_text, e.new_text);
        if (n > 0) return n;

        // Fallback: CRLF drift. If the buffer contains \r\n and the
        // needle doesn't (or vice versa), exact misses. Try both
        // CRLF and LF normalised needles.
        if (buf.find('\r') != std::string::npos
            || e.old_text.find('\r') != std::string::npos) {
            std::string alt_needle = with_crlf(without_cr(e.old_text));
            std::string alt_new    = with_crlf(without_cr(e.new_text));
            if (alt_needle != e.old_text) {
                n = replace_exact(alt_needle, alt_new);
                if (n > 0) return n;
            }
            std::string plain_needle = without_cr(e.old_text);
            std::string plain_new    = without_cr(e.new_text);
            if (plain_needle != e.old_text) {
                n = replace_exact(plain_needle, plain_new);
                if (n > 0) return n;
            }
        }

        // Last-chance: replace_all asked for *all* occurrences but exact +
        // CRLF found none. Fall through to fuzzy_find — if it lands a
        // single tolerant hit, honor it (better than failing the turn over
        // a whitespace nit). True ambiguity (count >= 2) still surfaces
        // below with the actionable line list.
        auto fm = util::fuzzy_find(buf, e.old_text, e.new_text, e.line_hint);
        if (fm.ok) {
            std::string_view repl = fm.adjusted_new_text.empty()
                ? std::string_view{e.new_text}
                : std::string_view{fm.adjusted_new_text};
            buf = minimal_splice(buf, fm.pos, fm.len, repl);
            return 1;
        }

        if (int ln = closest_line_hint(buf, e.old_text); ln > 0) {
            auto window = render_context_window(buf, ln, 2);
            err = std::format(
                "old_text not found in {} (replace_all tried exact, CRLF, "
                "and fuzzy strategies). The closest matching line is around "
                "line {} — here are the actual bytes at that location:\n"
                "```\n{}```\n"
                "Copy the snippet byte-for-byte from above and retry.",
                path_str, ln, window);
        }
        else
            err = "old_text not found in " + path_str
                + " (replace_all tried exact, CRLF, and fuzzy strategies). "
                  "Re-read the file and copy the snippet byte-for-byte.";
        return 0;
    }

    // ── Idempotency check ([was here, now hoisted above replace_all]) ──
    // The block previously sitting here was redundant; the hoisted
    // version handles both `replace_all` and single-match paths uniformly.

    // ── Exact-match fast path with CRLF fallback ────────────────────────
    // Before going through fuzzy_find, try CRLF-normalised exact
    // matching when there's any sign of line-ending drift. fuzzy_find
    // handles whitespace inside lines but doesn't reconcile \r\n vs
    // \n at end-of-line — a single missed \r in the needle on a CRLF
    // file produces a fuzzy_find ambiguity / miss that's painful to
    // debug. The exact match here is O(buf) and zero-allocation when
    // there's no drift; only the actual normalisation path allocates.
    if (!e.replace_all
        && (buf.find('\r') != std::string::npos
            || e.old_text.find('\r') != std::string::npos))
    {
        // Try \r-stripped needle against \r-stripped buffer. If exactly
        // one hit, splice using the buffer's original line endings
        // by re-rendering the replacement with the buffer's style.
        std::string plain_needle = without_cr(e.old_text);
        std::string plain_new    = without_cr(e.new_text);
        if (plain_needle != e.old_text) {
            // Quick count on plain_buf; if exactly one occurrence, do
            // an exact replace on the CRLF-aware variant.
            std::string plain_buf = without_cr(buf);
            std::size_t first = plain_buf.find(plain_needle);
            if (first != std::string::npos) {
                std::size_t next = plain_buf.find(plain_needle, first + plain_needle.size());
                if (next == std::string::npos) {
                    // Unique — try both line-ending styles on the real buf.
                    std::string crlf_needle = with_crlf(plain_needle);
                    std::string crlf_new    = with_crlf(plain_new);
                    int n = replace_exact(crlf_needle, crlf_new);
                    if (n == 1) return 1;
                    n = replace_exact(plain_needle, plain_new);
                    if (n == 1) return 1;
                }
            }
        }
    }

    auto m = util::fuzzy_find(buf, e.old_text, e.new_text, e.line_hint);
    if (!m.ok) {
        if (m.count >= 2) {
            // Surface where the duplicates live so the model can pick one
            // by adding context, instead of guessing in the dark.
            auto lines = hit_lines(buf, e.old_text, 5);
            std::string at;
            if (!lines.empty()) {
                at = " Matches at line";
                if (lines.size() > 1) at += "s";
                at += " " + join_ints(lines);
                if (m.count > static_cast<int>(lines.size()))
                    at += std::format(" (and {} more)", m.count - static_cast<int>(lines.size()));
                at += ".";
            }
            err = std::format(
                "old_text appears {} times in {}.{} Add surrounding context "
                "to make it unique, or pass replace_all=true.",
                m.count, path_str, at);
        } else {
            // Hint: indentation mismatch is the common cause. fuzzy_find
            // already tried both-sides-trim and whitespace-squash, so if
            // we still missed, the content likely isn't in the file at
            // all — but surface a hint anyway for the borderline cases.
            std::string hint;
            std::string squashed_old, squashed_buf;
            squashed_old.reserve(e.old_text.size());
            squashed_buf.reserve(buf.size());
            for (char c : e.old_text)
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                    squashed_old += c;
            for (char c : buf)
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                    squashed_buf += c;
            if (!squashed_old.empty()
                && squashed_buf.find(squashed_old) != std::string::npos) {
                // Locate roughly where the match landed in squashed-coords
                // and project back to a line number in the original buf.
                // We don't have a src_of[] here so we approximate: count
                // non-ws chars until the hit, then walk buf consuming
                // non-ws until the count is reached — cheap enough on
                // the failure path.
                auto sq_pos = squashed_buf.find(squashed_old);
                std::size_t consumed = 0, byte_pos = 0;
                for (; byte_pos < buf.size() && consumed < sq_pos; ++byte_pos) {
                    char c = buf[byte_pos];
                    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                        ++consumed;
                }
                int ln = line_of_offset(buf, byte_pos);
                hint = std::format(
                    " The text appears around line {} but differs in "
                    "whitespace/punctuation in a way fuzzy matching "
                    "couldn't reconcile — re-read the file at that line "
                    "and copy the snippet byte-for-byte.", ln);
            }
            if (hint.empty()) {
                if (int ln = closest_line_hint(buf, e.old_text); ln > 0) {
                    auto window = render_context_window(buf, ln, 2);
                    hint = std::format(
                        " The closest matching line is around line {} — "
                        "here are the actual bytes at that location:\n"
                        "```\n{}```\n"
                        "Copy the snippet byte-for-byte from above and retry.",
                        ln, window);
                }
            }
            err = "old_text not found in " + path_str + "." + hint;
        }
        return 0;
    }
    // Splice. Use the re-indented replacement when strategy 4 adjusted it
    // to match the file's indentation convention; otherwise the caller's
    // original new_text. We splice via minimal_splice so unchanged
    // prefix/suffix bytes inside the matched region survive byte-for-byte
    // — the resulting unified diff shows only the genuinely-changed run
    // instead of a wholesale block delete+insert.
    std::string_view repl =
        m.adjusted_new_text.empty()
            ? std::string_view{e.new_text}
            : std::string_view{m.adjusted_new_text};
    buf = minimal_splice(buf, m.pos, m.len, repl);
    return 1;
}

ExecResult run_edit(const EditArgs& a) {
    const auto& p = a.path.path();
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        // If the *parent* doesn't exist either, the model probably typed
        // a stale or wrong path — say so explicitly so it stops retrying
        // the same edit on a phantom file. If the parent exists, suggest
        // `write` (the canonical "create new file" tool).
        auto parent = p.parent_path();
        bool parent_ok = !parent.empty() && fs::exists(parent, ec);
        if (!parent_ok)
            return std::unexpected(ToolError::not_found(
                "file not found: " + a.path.string()
                + " (parent directory doesn't exist either). "
                  "Re-check the path — try `list_dir` on a directory you "
                  "know exists, or `glob` by filename."));
        return std::unexpected(ToolError::not_found(
            "file not found: " + a.path.string()
            + ". To create a new file use the `write` tool; "
              "`edit` only modifies existing files."));
    }
    if (!fs::is_regular_file(p, ec))
        return std::unexpected(ToolError::not_a_file(
            "not a regular file: " + a.path.string()
            + " (is it a directory or symlink to one?)"));

    // Refuse binary files. Edit's whole contract is line-oriented text
    // substitution; trying to run fuzzy_find on a 10 MB ELF blob is
    // both slow and meaningless, and any successful splice would just
    // corrupt the file. is_binary_file looks for NUL in the first 512 B
    // — cheap and catches every real-world binary.
    if (util::is_binary_file(p))
        return std::unexpected(ToolError::binary(
            "refusing to edit binary file: " + a.path.string()
            + " (contains NUL bytes — likely an image, archive, or compiled "
              "artifact). If this is a text file with a stray NUL, use "
              "`write` to rewrite it whole."));

    // Staleness hint: if a prior tool (read/edit/write) saw a snapshot
    // of this file and the on-disk state has drifted since, surface the
    // mismatch as a warning prefix on the output. We DON'T refuse the
    // edit — the fuzzy matcher may still land cleanly on the new bytes,
    // and false-failing a legitimate edit is worse than a soft warning.
    // But the model needs to know it might be working from stale context.
    std::string staleness_warning;
    if (util::staleness_of(p) == util::StaleVerdict::Stale) {
        staleness_warning =
            "⚠  The file has changed on disk since the last time a tool "
            "observed it this session. The edit was applied to the CURRENT "
            "bytes; if the result looks wrong, re-read the file before "
            "making further edits.\n\n";
    }

    std::string original = util::read_file(a.path);
    std::string updated  = original;

    // Best-effort batching: per-edit outcomes are collected; the file
    // is written if ANY edit applied successfully. The output reports
    // every per-edit result so the model knows exactly what landed and
    // what didn't. This is materially better than the previous all-or-
    // nothing semantic where one bad edit in a 5-edit batch caused all
    // four good ones to be discarded — the model would then re-emit
    // the entire batch, often re-introducing the failing edit.
    struct EditOutcome {
        std::size_t index;
        int  applied;       // >0 = replacements; 0 = failed; kIdempotentNoOp = already-present
        bool exact_count_mismatch;
        std::string err;
    };
    std::vector<EditOutcome> outcomes;
    outcomes.reserve(a.edits.size());
    std::size_t skipped_noop = 0;
    std::size_t applied = 0;
    std::size_t idempotent = 0;
    for (std::size_t i = 0; i < a.edits.size(); ++i) {
        const auto& ed = a.edits[i];
        if (ed.old_text == ed.new_text) { ++skipped_noop; continue; }
        std::string err;
        int n = apply_one(updated, ed, a.path.string(), err);

        if (n == kIdempotentNoOp) {
            // The edit's effect is already in the buffer (model is
            // re-applying). Soft-success: don't write anything new,
            // but don't fail either. Reported in the output so the
            // model sees "this one was already done".
            ++idempotent;
            outcomes.push_back({i, kIdempotentNoOp, false, {}});
            continue;
        }

        // Optional Anthropic-spec count enforcement.
        bool count_mismatch =
            (n > 0 && ed.expected_replacements > 0
             && n != ed.expected_replacements);
        if (count_mismatch) {
            // Don't write the partial result for a count mismatch —
            // the user pinned the count explicitly, so honour the
            // strict contract: roll back this edit. The fact that
            // `apply_one` already mutated `updated` means we need to
            // revert it.
            updated = original;
            // Re-apply every successful prior edit on the rolled-back
            // buffer so the partial-success picture stays consistent.
            for (const auto& prior : outcomes) {
                if (prior.applied <= 0) continue;
                std::string tmp_err;
                (void)apply_one(updated, a.edits[prior.index],
                                a.path.string(), tmp_err);
            }
            outcomes.push_back({i, n, true, std::format(
                "expected_replacements={} but matched {} occurrence(s)",
                ed.expected_replacements, n)});
            continue;
        }

        if (n == 0) {
            // Was old_text present in the ORIGINAL file? Distinguishes
            // "a prior edit clobbered the region" from "stale read".
            bool was_in_original = !original.empty()
                && original.find(ed.old_text) != std::string::npos;
            if (was_in_original && applied > 0) {
                err = std::format(
                    "{} The text WAS present in the file before this batch, "
                    "but {} earlier edit(s) altered the region. Re-order "
                    "this edit before the conflicting ones, OR merge them "
                    "into a single larger old_text.",
                    err, applied);
            }
            outcomes.push_back({i, 0, false, std::move(err)});
            continue;
        }

        outcomes.push_back({i, n, false, {}});
        ++applied;
    }

    // Summarise. The four buckets:
    //   applied        = edit landed N > 0 hits
    //   idempotent     = effect already in buffer (no-op)
    //   skipped_noop   = old == new (caller no-op)
    //   failed         = no-match / count-mismatch (err non-empty)
    std::size_t failed = 0;
    for (const auto& o : outcomes) if (o.applied == 0) ++failed;

    // All-no-op short circuits.
    if (applied == 0 && skipped_noop == a.edits.size()) {
        return ToolOutput{std::format(
            "{}No edits were applied — all {} edit(s) had identical "
            "old_text and new_text (nothing to change). File on disk is "
            "unchanged.", staleness_warning, a.edits.size()), std::nullopt};
    }
    if (applied == 0 && idempotent > 0 && failed == 0) {
        // Every requested edit was already in the file. The model is
        // "confirming" a state that's already reached. Tell it clearly
        // so it stops the retry loop.
        return ToolOutput{std::format(
            "{}No write needed — all {} edit(s) were already present in "
            "the file (new_text matched the on-disk bytes, old_text was "
            "absent). The desired state is already in place; move on.",
            staleness_warning, idempotent), std::nullopt};
    }

    // Some failures, but nothing landed. Fail the call entirely — the
    // model has nothing to act on. Surface every per-edit error so it
    // can rebuild the batch.
    if (applied == 0 && failed > 0) {
        std::string msg;
        if (a.edits.size() == 1) {
            msg = outcomes.front().err;
        } else {
            msg = std::format(
                "All {} edit(s) failed; file on disk is unchanged.",
                a.edits.size());
            for (const auto& o : outcomes) {
                if (o.applied == 0 && !o.err.empty())
                    msg += std::format("\n  edits[{}]: {}", o.index, o.err);
            }
        }
        // Preserve typed category when one edit's error message looks
        // ambiguous — keeps downstream error-class routing happy.
        for (const auto& o : outcomes) {
            if (o.applied == 0
                && o.err.find("appears") != std::string::npos)
                return std::unexpected(ToolError::ambiguous(std::move(msg)));
        }
        return std::unexpected(ToolError::no_match(std::move(msg)));
    }

    if (original == updated)
        return ToolOutput{staleness_warning
            + "No edits were made — all old_text / new_text pairs "
              "produced identical content (file unchanged on "
              "disk).", std::nullopt};

    auto change = diff::compute(a.path.string(), original, updated);
    if (auto werr = util::write_file(a.path, updated); !werr.empty())
        return std::unexpected(ToolError::io(werr));

    // Snapshot the new file state so subsequent tools don't false-alarm
    // on the change we just made.
    {
        std::error_code mt_ec;
        auto new_mtime = fs::last_write_time(p, mt_ec);
        if (!mt_ec) {
            util::record_file_seen(p, new_mtime,
                                   static_cast<std::uintmax_t>(updated.size()),
                                   util::content_fnv1a(updated));
        }
    }

    std::string unified = diff::render_unified(change);
    std::ostringstream msg;
    if (!staleness_warning.empty()) msg << staleness_warning;
    if (!a.display_description.empty())
        msg << a.display_description << "\n\n";
    msg << "Edited " << a.path.string() << " (" << change.added << "+ "
        << change.removed << "-";
    if (a.edits.size() > 1) {
        msg << ", " << applied << "/" << a.edits.size() << " edits applied";
        if (idempotent > 0)   msg << ", " << idempotent   << " already present";
        if (skipped_noop > 0) msg << ", " << skipped_noop << " no-op";
        if (failed > 0)       msg << ", " << failed       << " failed";
    }
    msg << "):\n\n```diff\n" << unified;
    if (unified.empty() || unified.back() != '\n') msg << "\n";
    msg << "```";

    // Per-edit detail when the batch had partial success. The model
    // needs to know which edits to re-emit (the failed ones) and which
    // to drop (the idempotent ones).
    if (a.edits.size() > 1 && (failed > 0 || idempotent > 0)) {
        msg << "\n\nPer-edit results:";
        for (const auto& o : outcomes) {
            msg << std::format("\n  edits[{}]: ", o.index);
            if (o.applied == kIdempotentNoOp)
                msg << "already present (no-op)";
            else if (o.applied > 0)
                msg << std::format("applied ({} replacement(s))", o.applied);
            else
                msg << "FAILED — " << o.err;
        }
        if (failed > 0) {
            msg << "\n\nThe successful edits have been written. To fix the "
                   "failed edits, retry ONLY those entries (don't re-emit "
                   "the ones already applied).";
        }
    }

    return ToolOutput{msg.str(), std::move(change)};
}

} // namespace

ToolDef tool_edit() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"edit">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Modify an EXISTING file by applying one or more targeted text "
        "substitutions. PREFER this tool over `write` whenever you are "
        "changing only part of a file — it streams less data and produces "
        "a reviewable diff. Pass `edits: [{old_text, new_text}, ...]`; "
        "every edit is applied in order. `old_text` is matched FUZZILY "
        "using line-level edit-distance — minor whitespace, indentation, "
        "smart-quote / dash, and single-character typo drift between the "
        "snippet you saw and the file on disk are tolerated automatically. "
        "Match the snippet as faithfully as you can; if `old_text` could "
        "plausibly refer to more than one region of the file, add "
        "surrounding context to disambiguate, OR pass `line: N` (1-based) "
        "to anchor the match near a known line. To replace multiple "
        "occurrences pass either `replace_all: true` (replace every hit) "
        "or `expected_replacements: N` (replace exactly N hits, fail if "
        "the count differs — safer for refactors). No-op edits (old_text "
        "== new_text) are silently skipped, not errors. Include a brief "
        "`display_description` (e.g. 'Fix null-deref in auth.cpp') — it "
        "shows in the card while edits stream.";
    // Property order matters for streaming UX (see write.cpp for context).
    // path → display_description → edits puts the small fields first so the
    // tool card paints meaningful content within ~1s of the model starting
    // to emit, rather than after the entire edits[] array streams.
    t.input_schema = json{
        {"type","object"},
        {"required", {"path","edits"}},
        {"properties", {
            {"path", {{"type","string"},
                {"description","Absolute or relative path of the existing "
                               "file. Stream this FIRST."}}},
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the card while "
                               "edits stream — e.g. 'Fix null-deref in "
                               "auth.cpp'. Stream second."}}},
            {"edits", {
                {"type","array"},
                {"description","One or more edits, applied in order."},
                {"items", {
                    {"type","object"},
                    {"required", {"old_text","new_text"}},
                    {"properties", {
                        {"old_text",  {{"type","string"},
                            {"description","Snippet to find. Matched "
                                "fuzzily by line edit-distance — minor "
                                "whitespace/indent drift and single-char "
                                "typos are tolerated. Should be unique "
                                "in the file OR accompanied by `line` "
                                "to disambiguate."}}},
                        {"new_text",  {{"type","string"},
                            {"description","Replacement text. "
                                "Indentation is auto-adjusted to match "
                                "the surrounding file convention when "
                                "old_text was at a different indent "
                                "level."}}},
                        {"replace_all",{{"type","boolean"},
                            {"default", false},
                            {"description","Replace every occurrence "
                                "instead of exactly one."}}},
                        {"expected_replacements",{{"type","integer"},
                            {"description","If set, edit must match "
                                "exactly this many times or it fails. "
                                "Safer than replace_all for known-count "
                                "refactors — surfaces mismatches as "
                                "errors instead of silent over-replace."}}},
                        {"line",{{"type","integer"},
                            {"description","1-based line number anchor "
                                "for ambiguous matches. If old_text "
                                "could match multiple regions, the one "
                                "closest to this line (within ~200 "
                                "lines) wins. Optional."}}},
                    }},
                }},
            }},
        }},
    };
    // See write.cpp for the full rationale. Edit's `edits[].new_text` can
    // also be multi-KB on big refactors; eager streaming keeps the
    // input_json_delta cadence matched to the model's emission rate.
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<EditArgs>(parse_edit_args, run_edit);
    return t;
}

} // namespace agentty::tools
