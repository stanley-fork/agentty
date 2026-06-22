// edit_minimal_splice_test — verifies the char-level minimal-splice path
// in the `edit` tool (src/tool/tools/edit.cpp `minimal_splice`).
//
// When an edit's old_text and new_text share a large common prefix and
// suffix (the usual case: change one line inside a multi-line block),
// the produced unified diff must show ONLY the genuinely-changed run —
// not a wholesale delete+re-insert of every line in old_text. This keeps
// review diffs tight and avoids perturbing unchanged trailing context.
//
// Drives the REAL registered `edit` ToolDef over fixture files in a temp
// workspace and inspects the returned ```diff``` body.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "agentty/tool/registry.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace agentty;

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_fails; }
    else     { std::fprintf(stderr, "ok:   %s\n", what); }
}

void write_file(const fs::path& p, const std::string& body) {
    std::ofstream o(p, std::ios::binary);
    o.write(body.data(), static_cast<std::streamsize>(body.size()));
}

std::string read_file(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    std::ostringstream ss; ss << i.rdbuf();
    return ss.str();
}

std::string run_edit(const json& args, bool& ok) {
    const auto* def = tools::find("edit");
    if (!def) { std::fprintf(stderr, "FAIL: edit not registered\n"); ++g_fails; ok = false; return {}; }
    auto r = def->execute(args);
    if (!r.has_value()) {
        std::fprintf(stderr, "  (edit errored: %s)\n", r.error().detail.c_str());
        ok = false;
        return {};
    }
    ok = true;
    return r->text;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Count how many lines in the rendered diff start with '+' or '-' (diff
// change markers — excludes the @@ hunk header and the +++/--- file
// header which we strip by only counting lines after the first @@).
int count_change_lines(const std::string& diff) {
    int n = 0;
    std::istringstream iss(diff);
    std::string line;
    bool in_hunk = false;
    while (std::getline(iss, line)) {
        if (line.rfind("@@", 0) == 0) { in_hunk = true; continue; }
        if (!in_hunk) continue;
        if (line.rfind("```", 0) == 0) break;
        if (!line.empty() && (line[0] == '+' || line[0] == '-')) ++n;
    }
    return n;
}

} // namespace

int main() {
    fs::path dir = fs::temp_directory_path()
                 / ("agentty_splice_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    tools::util::set_workspace_root(dir);

    // ── Test 1: one changed line inside a 6-line block ──────────────────
    // old_text spans the whole block; new_text differs by one token on
    // ONE line. The minimal splice must produce a diff with exactly one
    // '-' and one '+' (not six of each).
    {
        fs::path f = dir / "block.cpp";
        write_file(f,
            "int compute(int n) {\n"
            "    int total = 0;\n"
            "    for (int i = 0; i < n; ++i)\n"
            "        total += i;\n"
            "    return total;\n"
            "}\n");

        json args = {
            {"path", f.string()},
            {"edits", json::array({
                json{
                    {"old_text",
                        "int compute(int n) {\n"
                        "    int total = 0;\n"
                        "    for (int i = 0; i < n; ++i)\n"
                        "        total += i;\n"
                        "    return total;\n"
                        "}"},
                    {"new_text",
                        "int compute(int n) {\n"
                        "    int total = 0;\n"
                        "    for (int i = 0; i < n; ++i)\n"
                        "        total += i * 2;\n"
                        "    return total;\n"
                        "}"},
                },
            })},
        };
        bool ok = false;
        std::string out = run_edit(args, ok);
        check(ok, "block edit applied");
        int changes = count_change_lines(out);
        check(changes <= 2,
              "single-line block edit yields minimal diff (<=2 change lines)");
        // The change line must reference the actual edit, not the
        // unchanged 'return total;' line.
        check(contains(out, "total += i * 2;"),
              "diff shows the changed line");
        // And the file on disk must be correct.
        std::string disk = read_file(f);
        check(contains(disk, "total += i * 2;")
              && contains(disk, "int total = 0;")
              && contains(disk, "return total;"),
              "disk content correct after minimal splice");
    }

    // ── Test 2: changed token in the MIDDLE of a single long line ───────
    // Shared prefix and suffix on the same line. Result must still be
    // byte-correct (no UTF-8 / boundary corruption).
    {
        fs::path f = dir / "line.txt";
        write_file(f, "const greeting = \"hello world\" + suffix;\n");
        json args = {
            {"path", f.string()},
            {"edits", json::array({
                json{
                    {"old_text", "const greeting = \"hello world\" + suffix;"},
                    {"new_text", "const greeting = \"howdy world\" + suffix;"},
                },
            })},
        };
        bool ok = false;
        std::string out = run_edit(args, ok);
        check(ok, "mid-line edit applied");
        std::string disk = read_file(f);
        check(disk == "const greeting = \"howdy world\" + suffix;\n",
              "mid-line splice byte-correct");
    }

    // ── Test 3: UTF-8 multibyte content — splice must not corrupt ───────
    // Shared multibyte prefix/suffix around an ASCII change.
    {
        fs::path f = dir / "utf8.txt";
        // "café — value" → change 'value' to 'result', keeping the
        // multibyte é and em-dash around it.
        write_file(f, "café — value here\n");
        json args = {
            {"path", f.string()},
            {"edits", json::array({
                json{
                    {"old_text", "café — value here"},
                    {"new_text", "café — result here"},
                },
            })},
        };
        bool ok = false;
        std::string out = run_edit(args, ok);
        check(ok, "utf8 edit applied");
        std::string disk = read_file(f);
        check(disk == "café — result here\n",
              "utf8 splice preserves multibyte bytes exactly");
    }

    // ── Test 4: pure prefix change (suffix shared) ──────────────────────
    {
        fs::path f = dir / "prefix.txt";
        write_file(f, "AAA shared tail\n");
        json args = {
            {"path", f.string()},
            {"edits", json::array({
                json{
                    {"old_text", "AAA shared tail"},
                    {"new_text", "BBB shared tail"},
                },
            })},
        };
        bool ok = false;
        std::string out = run_edit(args, ok);
        check(ok, "prefix-change edit applied");
        check(read_file(f) == "BBB shared tail\n", "prefix-change correct");
    }

    // ── Test 5: full replacement (nothing shared) still works ───────────
    {
        fs::path f = dir / "whole.txt";
        write_file(f, "xxxxx\n");
        json args = {
            {"path", f.string()},
            {"edits", json::array({
                json{ {"old_text", "xxxxx"}, {"new_text", "yyyyy"} },
            })},
        };
        bool ok = false;
        std::string out = run_edit(args, ok);
        check(ok, "no-shared edit applied");
        check(read_file(f) == "yyyyy\n", "full-replacement correct");
    }

    if (g_fails == 0) std::fprintf(stderr, "\nALL PASS\n");
    else              std::fprintf(stderr, "\n%d FAIL(s)\n", g_fails);
    return g_fails ? 1 : 0;
}
