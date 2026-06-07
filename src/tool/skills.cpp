// agentty::tools::skills — implementation. See skills.hpp for the
// progressive-disclosure rationale.

#include "agentty/tool/skills.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

namespace agentty::tools::skills {

namespace fs = std::filesystem;

namespace {

[[nodiscard]] fs::path home_dir() noexcept {
    if (auto* h = std::getenv("HOME"); h && *h) return fs::path{h};
#if defined(_WIN32)
    if (auto* h = std::getenv("USERPROFILE"); h && *h) return fs::path{h};
#endif
    return {};
}

// Trim ASCII whitespace from both ends.
[[nodiscard]] std::string trim(std::string s) {
    auto issp = [](char c) { return c==' '||c=='\t'||c=='\r'||c=='\n'; };
    while (!s.empty() && issp(s.front())) s.erase(s.begin());
    while (!s.empty() && issp(s.back()))  s.pop_back();
    return s;
}

// Read a file with a hard byte cap. Empty on missing / unreadable / oversize.
[[nodiscard]] std::string read_capped(const fs::path& p, std::size_t cap) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return {};
    auto sz = fs::file_size(p, ec);
    if (ec || sz == 0 || sz > cap) return {};
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string out(static_cast<std::size_t>(sz), '\0');
    f.read(out.data(), static_cast<std::streamsize>(sz));
    out.resize(static_cast<std::size_t>(f.gcount()));
    return out;
}

// Parse `key: value` from a YAML frontmatter line. Returns false if the
// line isn't a simple scalar mapping.
[[nodiscard]] bool parse_kv(const std::string& line,
                            std::string& key, std::string& val) {
    auto colon = line.find(':');
    if (colon == std::string::npos) return false;
    key = trim(line.substr(0, colon));
    val = trim(line.substr(colon + 1));
    // Strip matching surrounding quotes on the value.
    if (val.size() >= 2 &&
        ((val.front() == '"' && val.back() == '"') ||
         (val.front() == '\'' && val.back() == '\''))) {
        val = val.substr(1, val.size() - 2);
    }
    return !key.empty();
}

// Split a SKILL.md into (name, description, body). Frontmatter is the
// block between the first two `---` lines; only `name` + `description`
// are read. `slug` is the directory name, used as the name fallback.
[[nodiscard]] Skill parse_skill(const std::string& raw, const std::string& slug,
                                const std::string& source) {
    Skill s;
    s.name = slug;
    s.source = source;

    std::istringstream in(raw);
    std::string line;
    // Detect frontmatter: first non-empty line must be exactly "---".
    std::streampos body_start = 0;
    bool in_fm = false;
    bool fm_done = false;
    std::string first;
    if (std::getline(in, first) && trim(first) == "---") {
        in_fm = true;
        while (std::getline(in, line)) {
            if (trim(line) == "---") { fm_done = true; body_start = in.tellg(); break; }
            std::string k, v;
            if (parse_kv(line, k, v)) {
                if (k == "name" && !v.empty())        s.name = v;
                else if (k == "description")          s.description = v;
            }
        }
    }
    (void)in_fm;

    if (fm_done && body_start != std::streampos(-1)) {
        s.body = trim(raw.substr(static_cast<std::size_t>(body_start)));
    } else {
        // No frontmatter — treat the whole file as the body, first
        // non-blank line doubles as the description.
        s.body = trim(raw);
        std::istringstream b(s.body);
        std::string l;
        while (std::getline(b, l)) {
            auto t = trim(l);
            if (!t.empty()) { s.description = t; break; }
        }
    }
    return s;
}

// Scan one root for <slug>/SKILL.md entries, appending to `out` (skipping
// names already present so project shadows user). Records the root's
// directory mtime into `sig` for cache invalidation.
void scan_root(const fs::path& root, const std::string& source,
               std::vector<Skill>& out, std::string& sig) {
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) return;
    auto mt = fs::last_write_time(root, ec);
    if (!ec) sig += source + ":" + std::to_string(mt.time_since_epoch().count()) + ";";

    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (const auto& d : dirs) {
        if (out.size() >= kMaxSkills) break;
        fs::path md = d / "SKILL.md";
        std::string raw = read_capped(md, kMaxBodyBytes);
        if (raw.empty()) continue;
        Skill s = parse_skill(raw, d.filename().string(), source);
        if (s.name.empty()) continue;
        // Shadow: a project skill with the same name as a user one wins.
        // We scan project first, so skip if name already present.
        if (std::ranges::any_of(out, [&](const Skill& e){ return e.name == s.name; }))
            continue;
        out.push_back(std::move(s));
    }
}

std::vector<Skill>& cache() {
    static std::vector<Skill> c;
    return c;
}

} // namespace

const std::vector<Skill>& all() {
    static std::mutex mu;
    static std::string cached_sig = "\x01uninit";
    std::lock_guard lk(mu);

    // Build the current signature from both roots' mtimes; rescan only
    // when it changed (cheap stat vs full parse on every turn).
    std::string sig;
    std::vector<Skill> fresh;
    // Project first so it shadows user on name collision.
    scan_root(fs::path{".agentty"} / "skills", "project", fresh, sig);
    scan_root(home_dir() / ".agentty" / "skills", "user", fresh, sig);

    if (sig != cached_sig) {
        cache() = std::move(fresh);
        cached_sig = sig;
    }
    return cache();
}

const Skill* find(std::string_view name) {
    for (const auto& s : all())
        if (s.name == name) return &s;
    return nullptr;
}

std::string catalog_block() {
    const auto& skills = all();
    if (skills.empty()) return {};
    std::ostringstream m;
    m << "\n\n<skills>\n"
      << "On-demand skills are available. Each is a focused instruction "
         "doc you can load IN FULL with the `skill` tool when its task "
         "comes up \u2014 don't guess the contents, load it. Listed: name "
         "\u2014 description.\n";
    for (const auto& s : skills) {
        m << "- " << s.name;
        if (!s.description.empty()) m << " \u2014 " << s.description;
        m << "\n";
    }
    m << "</skills>";
    return m.str();
}

} // namespace agentty::tools::skills
