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

// Apply a single edit to `buf`. Returns number of replacements; sets `err`
// on terminal failure (ambiguous match, not found).
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

    if (e.replace_all) {
        int n = replace_exact(e.old_text, e.new_text);
        if (n > 0) return n;

        // Fallback: CRLF drift. If the buffer contains \r\n and the
        // needle doesn't (or vice versa), exact misses. Try the match
        // with both sides \r-stripped, and synthesize a replacement that
        // re-emits the original line ending style of each hit. We do
        // this by locating hits in the stripped view and projecting
        // the splice range back via the src_of[] mapping — keeping the
        // surrounding bytes (including any \r) identical.
        if (buf.find('\r') != std::string::npos
            || e.old_text.find('\r') != std::string::npos) {
            // Cheap re-normalize inline to avoid pulling more of the
            // fuzzy_match internals into a public API. Strip \r from the
            // needle, replace \r\n→\n in the buffer temporarily, do the
            // exact replace, and finally un-normalize is not safe. Simpler:
            // do two exact passes — with \r\n needle variant, then with
            // plain \n needle variant — and apply whichever hits.
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
            auto without_cr = [](std::string_view s) {
                std::string out;
                out.reserve(s.size());
                for (char c : s) if (c != '\r') out.push_back(c);
                return out;
            };

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
            std::string out;
            out.reserve(buf.size() - fm.len + repl.size());
            out.append(buf, 0, fm.pos);
            out.append(repl);
            out.append(buf, fm.pos + fm.len, std::string::npos);
            buf = std::move(out);
            return 1;
        }

        if (int ln = closest_line_hint(buf, e.old_text); ln > 0)
            err = std::format(
                "old_text not found in {} (replace_all tried exact, CRLF, "
                "and fuzzy strategies). The closest matching line is around "
                "line {} \u2014 re-read that region and copy the snippet "
                "byte-for-byte.", path_str, ln);
        else
            err = "old_text not found in " + path_str
                + " (replace_all tried exact, CRLF, and fuzzy strategies). "
                  "Re-read the file and copy the snippet byte-for-byte.";
        return 0;
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
                if (int ln = closest_line_hint(buf, e.old_text); ln > 0)
                    hint = std::format(
                        " The closest matching line is around line {} \u2014 "
                        "re-read that region and copy the snippet byte-for-byte.",
                        ln);
            }
            err = "old_text not found in " + path_str + "." + hint;
        }
        return 0;
    }
    // Splice. Use the re-indented replacement when strategy 4 adjusted it
    // to match the file's indentation convention; otherwise the caller's
    // original new_text.
    std::string_view repl =
        m.adjusted_new_text.empty()
            ? std::string_view{e.new_text}
            : std::string_view{m.adjusted_new_text};
    std::string out;
    out.reserve(buf.size() - m.len + repl.size());
    out.append(buf, 0, m.pos);
    out.append(repl);
    out.append(buf, m.pos + m.len, std::string::npos);
    buf = std::move(out);
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

    std::string original = util::read_file(a.path);
    std::string updated  = original;

    // Apply in order. Identical old==new is silently skipped (it's a no-op,
    // not worth failing a batch over). If any other edit fails, report
    // which one, whether prior edits had already landed, and whether the
    // failing old_text was visible in the pre-batch buffer — the model
    // needs that distinction to decide between "re-read" and "re-order".
    std::size_t skipped_noop = 0;
    std::size_t applied = 0;
    for (std::size_t i = 0; i < a.edits.size(); ++i) {
        const auto& ed = a.edits[i];
        if (ed.old_text == ed.new_text) { ++skipped_noop; continue; }
        std::string err;
        int n = apply_one(updated, ed, a.path.string(), err);

        // Optional Anthropic-spec count enforcement. When the caller pinned
        // `expected_replacements`, demand exactly that many hits. n==0
        // already became an error above; here we catch the over/under hit.
        if (n > 0 && ed.expected_replacements > 0
            && n != ed.expected_replacements) {
            std::string ctx = a.edits.size() == 1
                ? std::format("expected_replacements={} but matched {} occurrence(s) in {}. "
                              "Adjust expected_replacements or add context to old_text.",
                              ed.expected_replacements, n, a.path.string())
                : std::format("edits[{}]: expected_replacements={} but matched {} "
                              "occurrence(s) in {} (prior {} edit(s) already applied to the buffer).",
                              i, ed.expected_replacements, n, a.path.string(), applied);
            return std::unexpected(ToolError::ambiguous(std::move(ctx)));
        }

        if (n == 0) {
            // Was this old_text present in the ORIGINAL file? If so, a
            // prior edit in this batch likely overwrote the region —
            // a sequencing problem, not a stale read. Surface that
            // distinction so the model retries the right way.
            bool was_in_original = !original.empty()
                && original.find(ed.old_text) != std::string::npos;
            std::string ctx;
            if (a.edits.size() == 1) {
                ctx = std::move(err);
            } else if (was_in_original && applied > 0) {
                ctx = std::format(
                    "edits[{}]: {} The text WAS present in the file before "
                    "this batch, but {} earlier edit(s) altered the region. "
                    "Re-order the edits (apply this one first), merge them "
                    "into a single larger old_text, or run them in separate "
                    "tool calls.",
                    i, err, applied);
            } else {
                ctx = std::format("edits[{}]: {}{}", i, err,
                    applied > 0
                        ? std::format(" ({} earlier edit(s) had already applied; "
                                      "the file is now partially modified in memory "
                                      "but NOT on disk \u2014 nothing was written.)",
                                      applied)
                        : std::string{});
            }
            // Preserve the error category based on the message shape.
            if (err.find("appears") != std::string::npos)
                return std::unexpected(ToolError::ambiguous(std::move(ctx)));
            return std::unexpected(ToolError::no_match(std::move(ctx)));
        }
        ++applied;
    }

    // All edits were no-ops? Tell the model clearly so it stops retrying.
    if (applied == 0 && skipped_noop == a.edits.size())
        return ToolOutput{std::format(
            "No edits were applied \u2014 all {} edit(s) had identical "
            "old_text and new_text (nothing to change). File on disk is "
            "unchanged.", a.edits.size()), std::nullopt};

    if (original == updated)
        return ToolOutput{"No edits were made \u2014 all old_text / new_text pairs "
                          "produced identical content (file unchanged on "
                          "disk).", std::nullopt};

    auto change = diff::compute(a.path.string(), original, updated);
    if (auto werr = util::write_file(a.path, updated); !werr.empty())
        return std::unexpected(ToolError::io(werr));

    std::string unified = diff::render_unified(change);
    std::ostringstream msg;
    if (!a.display_description.empty())
        msg << a.display_description << "\n\n";
    msg << "Edited " << a.path.string() << " (" << change.added << "+ "
        << change.removed << "-";
    if (a.edits.size() > 1) {
        msg << ", " << a.edits.size() << " edits";
        if (skipped_noop > 0)
            msg << " (" << skipped_noop << " skipped as no-op)";
    }
    msg << "):\n\n```diff\n" << unified;
    if (unified.empty() || unified.back() != '\n') msg << "\n";
    msg << "```";
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
