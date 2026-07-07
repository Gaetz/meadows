#include "data/plugins/Synthesis.hpp"

#include <algorithm>

#include "data/forms/FormDatabase.hpp"
#include "data/plugins/TomlWriter.hpp"

namespace data {

Plugin makeSynthesisPatch(const core::Guid& pluginId, const str& name,
                          const vector<SynthesisChoice>& choices,
                          const vector<core::Guid>& dependencies) {
    Plugin plugin;
    plugin.id = pluginId;
    plugin.name = name;
    plugin.dependencies = dependencies;

    for (const SynthesisChoice& choice : choices) {
        auto it = std::find_if(plugin.records.begin(), plugin.records.end(),
                               [&](const Record& record) {
                                   return record.formId == choice.formId;
                               });
        if (it == plugin.records.end()) {
            plugin.records.push_back(
                { choice.formId, choice.typeId, /*creates=*/false, {} });
            it = std::prev(plugin.records.end());
        }
        it->fields[choice.fieldId] = choice.value;
    }
    std::sort(plugin.records.begin(), plugin.records.end(),
              [](const Record& a, const Record& b) {
                  return a.formId < b.formId;
              });
    return plugin;
}

str writeSynthesisToml(const Plugin& plugin, const FormTypeRegistry& types,
                       const vector<SynthesisChoice>& choices,
                       const FormDatabase* database) {
    // Provenance header, sorted for stable diffs. TOML comments — the
    // parser skips them, a future regeneration pass reads them.
    vector<str> lines;
    lines.reserve(choices.size());
    for (const SynthesisChoice& choice : choices) {
        str formName = choice.formId.toString();
        if (database) {
            if (const Form* form = database->find(choice.formId);
                form && !form->editorId.empty()) {
                formName = form->editorId;
            }
        }
        lines.push_back(
            "# " + formName + "." + choice.fieldName + " from: " +
            (choice.provenance.empty() ? str { "custom" }
                                       : choice.provenance));
    }
    std::sort(lines.begin(), lines.end());

    str out = "# synthesis patch (generated) - provenance:\n";
    for (const str& line : lines) {
        out += line + "\n";
    }
    out += "\n";
    out += writePluginToml(plugin, types);
    return out;
}

} // namespace data
