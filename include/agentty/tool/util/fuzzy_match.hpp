#pragma once
// Line-DP fuzzy matching for the edit tool, ported from Zed's
// StreamingFuzzyMatcher.
//
// The model's `old_text` often disagrees with the file in trivial ways:
// trailing spaces, tab/space drift, an extra blank line, a one-character
// typo, indentation level differences. Exact-substring matching forces a
// retry loop on every one of these; the right thing is to treat `old_text`
// as a *fuzzy line query* and find the file region with minimum edit
// distance under a line-level cost model.
//
// Algorithm (Wagner-Fischer over LINES, not bytes):
//   • Both file and needle are split into lines; lines are trimmed before
//     comparison so leading/trailing whitespace is free.
//   • Each DP cell is min(up + DELETION_COST,           // skip a query line
//                          left + INSERTION_COST,        // skip a buffer line
//                          diag + (line match cost))    // align line pair.
//   • Line match cost: 0 if trimmed lines are byte-equal; REPLACEMENT_COST
//     if normalized Levenshtein >= 0.8 (typo-tolerant); else DELETION+INSERTION.
//   • Asymmetric weights bias toward complete-query matches: it's cheap
//     for the buffer to have extra lines, expensive for the query to.
//   • Result is the column with minimum cost in the final row; ties are
//     broken by `line_hint` (closest start row within 200 lines), or
//     reported as ambiguous when no hint is supplied.
//   • Match accepted only if matched_lines / max(buf_rows, query_lines) >= 0.8 —
//     filters spurious low-quality matches when the query genuinely isn't there.
//
// One algorithm subsumes the prior 5-strategy ladder (exact / CRLF /
// trailing-ws / both-sides-trim / unicode-squash) and additionally handles
// per-character typos and missing/extra lines.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace agentty::tools::util {

struct FuzzyMatch {
    bool        ok;      // true iff exactly one usable match (or one survived line_hint)
    std::size_t pos;     // byte offset into file (0 if !ok)
    std::size_t len;     // bytes of file that correspond to `needle`
    int         count;   // total matches seen (1 on ok; >1 means ambiguous)
    // When the match is at a different indentation level than the needle,
    // `new_text` is re-indented to match the file's convention and stored
    // here. Empty means the caller should splice the original `new_text`
    // unchanged. (Indent fix-up runs on every DP match where the needle's
    // first non-blank line indent differs from the matched region's.)
    std::string adjusted_new_text;
    // 0 = none, 1 = exact, 2 = DP. Kept for diagnostics; callers ignore.
    int         strategy = 0;
};

// Convenience overload — back-compat for callers that don't need the
// replacement re-indent or line hint.
FuzzyMatch fuzzy_find(std::string_view file, std::string_view needle);

// Mid-fidelity API — also accepts the intended replacement so the
// returned match can carry an indent-adjusted `new_text` when needed.
FuzzyMatch fuzzy_find(std::string_view file,
                      std::string_view needle,
                      std::string_view new_text);

// Full-fidelity API. `line_hint` is the 0-based row in `file` where the
// caller expects the match to land. When the DP produces multiple matches
// tied for best cost, the one closest to `line_hint` within a 200-line
// tolerance wins; without a hint, the call returns count>=2 and !ok.
// Pass UINT32_MAX (the default) for "no hint".
FuzzyMatch fuzzy_find(std::string_view file,
                      std::string_view needle,
                      std::string_view new_text,
                      std::uint32_t    line_hint);

} // namespace agentty::tools::util
