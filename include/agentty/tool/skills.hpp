#pragma once
// agentty::tools::skills — on-demand instruction docs (the "skills"
// pattern: progressive disclosure under a token budget).
//
// A skill is a directory holding a `SKILL.md` with YAML frontmatter:
//
//     ---
//     name: pdf-extract
//     description: Extract text + tables from PDF files using pdfplumber.
//     ---
//     <full markdown body: detailed instructions, code snippets, ...>
//
// Discovery roots (both optional):
//   • <cwd>/.agentty/skills/<slug>/SKILL.md   (project)
//   • ~/.agentty/skills/<slug>/SKILL.md       (user)
//
// The system prompt carries only the CATALOG (name + one-line
// description) — cheap. The agent pulls a skill's full body on demand
// via the `skill` tool. This is the same progressive-disclosure design
// Claude Code / Zed use: the model decides what to load instead of
// every skill's full text bloating every request.

#include <string>
#include <vector>

namespace agentty::tools::skills {

struct Skill {
    std::string name;         // frontmatter `name` (or slug fallback)
    std::string description;  // frontmatter `description` (one line)
    std::string body;         // markdown after the frontmatter
    std::string source;       // "project" | "user" (provenance for the catalog)
};

// Discover + parse every skill under the project + user roots. Bounded:
// at most kMaxSkills entries, each body capped at kMaxBodyBytes. Result
// is cached process-wide keyed by the roots' mtimes (cheap re-scan when
// nothing changed). Project skills shadow user skills with the same name.
[[nodiscard]] const std::vector<Skill>& all();

// Look up one skill by exact name. nullptr if absent.
[[nodiscard]] const Skill* find(std::string_view name);

// Render the compact catalog block for the system prompt:
//   <skills>
//   Available skills (load full instructions with the `skill` tool):
//   - name: description
//   ...
//   </skills>
// Empty string when no skills exist (no block emitted).
[[nodiscard]] std::string catalog_block();

inline constexpr std::size_t kMaxSkills    = 64;
inline constexpr std::size_t kMaxBodyBytes = 64 * 1024;

} // namespace agentty::tools::skills
