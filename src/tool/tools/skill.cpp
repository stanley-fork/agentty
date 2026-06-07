// skill — load one on-demand skill's full instruction doc.
//
// The system prompt carries only the skill catalog (name + one-line
// description). When a skill's task comes up the model calls this tool
// with the skill name; the full SKILL.md body is returned as the tool
// result for the model to follow. Progressive disclosure: nothing but
// the cheap catalog rides every request.

#include "agentty/tool/skills.hpp"
#include "agentty/tool/spec.hpp"
#include "agentty/tool/tool.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/tool_args.hpp"

#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace agentty::tools {

using json = nlohmann::json;

namespace {

struct SkillArgs {
    std::string name;
    std::string display_description;
};

std::expected<SkillArgs, ToolError> parse_skill_args(const json& j) {
    util::ArgReader ar(j);
    SkillArgs out;
    out.name = ar.str("name", "");
    out.display_description = ar.str("display_description", "");
    if (out.name.empty())
        return std::unexpected(ToolError::invalid_args(
            "skill requires the `name` of the skill to load"));
    return out;
}

ExecResult run_skill(const SkillArgs& a) {
    const auto* s = skills::find(a.name);
    if (!s) {
        // Help the model recover: list the names that DO exist.
        std::ostringstream avail;
        bool first = true;
        for (const auto& sk : skills::all()) {
            avail << (first ? "" : ", ") << sk.name;
            first = false;
        }
        std::string msg = "no skill named '" + a.name + "'";
        if (!first) msg += " — available: " + avail.str();
        else        msg += " — no skills are installed in this workspace";
        return std::unexpected(ToolError::not_found(std::move(msg)));
    }
    std::ostringstream out;
    out << "# Skill: " << s->name;
    if (!s->description.empty()) out << "\n" << s->description;
    out << "\n\n" << s->body;
    return ToolOutput{out.str(), std::nullopt};
}

} // namespace

ToolDef tool_skill() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"skill">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Load the full instructions for a named skill (see the <skills> "
        "catalog in your system prompt). Call this BEFORE attempting a "
        "task a skill covers — the catalog only lists names + summaries; "
        "the real procedure lives in the skill body this returns.";
    t.input_schema = json{
        {"type", "object"},
        {"required", {"name"}},
        {"properties", {
            {"name", {{"type", "string"},
                {"description", "Exact skill name from the <skills> catalog."}}},
            {"display_description", {{"type", "string"},
                {"description", "One-line summary shown in the UI. Optional."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<SkillArgs>(parse_skill_args, run_skill);
    return t;
}

} // namespace agentty::tools
