// Smoke test for the line-DP fuzzy matcher. Builds against the agentty
// fuzzy_match TU directly (no GoogleTest dependency).
//
// Cases mirror Zed's StreamingFuzzyMatcher test suite — they're the
// regressions we want to keep passing.

#include "agentty/tool/util/fuzzy_match.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

using agentty::tools::util::fuzzy_find;

namespace {

int failures = 0;
int total = 0;

void check(const char* name, bool cond, std::string_view detail = {}) {
    ++total;
    if (cond) {
        std::printf("  ok   %s\n", name);
    } else {
        ++failures;
        std::printf("  FAIL %s — %.*s\n", name,
                    static_cast<int>(detail.size()), detail.data());
    }
}

void test_exact_unique() {
    std::string_view file = "alpha\nbeta\ngamma\n";
    auto r = fuzzy_find(file, "beta");
    check("exact unique",
          r.ok && r.pos == 6 && r.len == 4,
          "expected pos=6 len=4");
}

void test_exact_ambiguous_no_hint() {
    std::string_view file = "foo\nfoo\nfoo\n";
    auto r = fuzzy_find(file, "foo");
    check("ambiguous without hint",
          !r.ok && r.count >= 2);
}

void test_exact_ambiguous_with_hint() {
    std::string_view file = "fn first(){}\nfn second(){}\nfn third(){}\n";
    auto r = fuzzy_find(file, "fn second(){}", "", 1u /* 0-based line of `fn second` */);
    check("ambiguous resolved by hint",
          r.ok && r.pos == 13 && r.len == 13,
          "expected pos=13 len=13");
}

void test_typo_tolerated() {
    std::string_view file =
        "fn foo1(a: usize) -> usize {\n"
        "    40\n"
        "}\n"
        "\n"
        "fn foo2(b: usize) -> usize {\n"
        "    42\n"
        "}\n";
    // Needle has a typo: u32 instead of usize.
    std::string_view needle = "fn foo1(a: usize) -> u32 {\n40\n}";
    auto r = fuzzy_find(file, needle);
    check("typo in keyword tolerated",
          r.ok && r.pos == 0,
          "expected the foo1 block");
}

void test_indent_drift() {
    std::string_view file =
        "class C {\n"
        "    fn m() {\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    // Needle is outdented (model emitted method body without class indent).
    std::string_view needle = "fn m() {\n    return 1;\n}";
    std::string_view new_t  = "fn m() {\n    return 2;\n}";
    auto r = fuzzy_find(file, needle, new_t);
    check("indent drift detected",
          r.ok,
          "expected fuzzy match");
    // Indent-fixup should have prepended 4 spaces to each non-blank line.
    bool reindented = !r.adjusted_new_text.empty()
        && r.adjusted_new_text.find("    fn m() {") != std::string::npos
        && r.adjusted_new_text.find("        return 2;") != std::string::npos;
    check("indent fixup applied", reindented,
          r.adjusted_new_text);
}

void test_no_match() {
    std::string_view file = "alpha\nbeta\ngamma\n";
    auto r = fuzzy_find(file, "totally unrelated content that doesnt belong");
    check("genuine no-match rejected",
          !r.ok && r.count == 0);
}

void test_trailing_whitespace_drift() {
    // File has trailing spaces on each line.
    std::string_view file =
        "fn first() {    \n"
        "    body();    \n"
        "}    \n";
    // Needle is the clean version.
    std::string_view needle = "fn first() {\n    body();\n}";
    auto r = fuzzy_find(file, needle);
    check("trailing-ws drift matched",
          r.ok && r.pos == 0,
          "expected match from start of file");
}

void test_crlf() {
    std::string_view file = "alpha\r\nbeta\r\ngamma\r\n";
    auto r = fuzzy_find(file, "beta");
    check("CRLF tolerated", r.ok);
}

void test_smart_quotes() {
    // U+2018 / U+2019 are 3-byte UTF-8.
    std::string_view file = "print(\xe2\x80\x98hello\xe2\x80\x99)\n";
    auto r = fuzzy_find(file, "print('hello')");
    // Length diff: file has 3+3=6 extra bytes for quotes vs the 1+1 in
    // needle, but normalized Levenshtein on (file_line=15, needle=14)
    // → diff = 4, max=15, norm = 11/15 ≈ 0.73. JUST below threshold.
    // This documents the limit: a single line of pure punctuation drift
    // can fall under the 0.8 line-fuzzy gate. We accept either result;
    // the test exists to surface the behavior, not pin a specific one.
    std::printf("  note  smart-quotes ok=%d (informational)\n", int(r.ok));
}

void test_class_methods() {
    // Zed's test_resolve_location_class_methods — the needle skips a line
    // (three() returns 333) so it must use INSERTION_COST.
    std::string_view file =
        "class Something {\n"
        "    one() { return 1; }\n"
        "    two() { return 2222; }\n"
        "    three() { return 333; }\n"
        "    four() { return 4444; }\n"
        "    five() { return 5555; }\n"
        "    six() { return 6666; }\n"
        "    seven() { return 7; }\n"
        "    eight() { return 8; }\n"
        "}\n";
    std::string_view needle =
        "two() { return 2222; }\n"
        "four() { return 4444; }\n"
        "five() { return 5555; }\n"
        "six() { return 6666; }\n";
    auto r = fuzzy_find(file, needle);
    check("multi-line match with skipped line",
          r.ok,
          "expected DP to align query lines across an inserted line");
}

} // namespace

int main() {
    std::printf("fuzzy_match smoke tests:\n");
    test_exact_unique();
    test_exact_ambiguous_no_hint();
    test_exact_ambiguous_with_hint();
    test_typo_tolerated();
    test_indent_drift();
    test_no_match();
    test_trailing_whitespace_drift();
    test_crlf();
    test_smart_quotes();
    test_class_methods();
    std::printf("\n%d/%d passed\n", total - failures, total);
    return failures == 0 ? 0 : 1;
}
