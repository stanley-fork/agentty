#include "agentty/runtime/view/status_bar/model_badge.hpp"

#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"

namespace agentty::ui {

maya::ModelBadge model_badge_config(const Model& m) {
    // Prefix the model id with the provider label when the active backend
    // isn't the default (Anthropic). The ModelBadge widget colour-codes
    // Claude families; for OpenAI/Groq/Ollama/etc. an explicit "groq · "
    // prefix tells the user which backend they're talking to at a glance —
    // the model id alone (e.g. "llama-3.3-70b") doesn't carry that.
    std::string label = m.d.model_id.value;
    const auto& sel = provider::active();
    if (sel.kind == provider::Kind::OpenAI) {
        const std::string& lbl = sel.openai_endpoint.label;
        // Look up the human display name from the registry when available
        // (e.g. label "groq" → "Groq"); fall back to the raw label for a
        // custom host that has no preset row.
        std::string_view shown = lbl;
        if (const auto* p = provider::preset_for(lbl)) shown = p->label;
        label = std::string{shown} + " · " + label;
    }
    maya::ModelBadge mb{std::move(label)};
    mb.set_compact(true);
    return mb;
}

} // namespace agentty::ui
