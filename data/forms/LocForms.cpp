#include "data/forms/LocForms.hpp"

#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"

namespace data {

void registerLocFormTypes(FormTypeRegistry& registry) {
    registry.registerFormType<LocStringForm>();
}

void TextTable::build(const FormDatabase& forms) {
    entries.clear();
    forEach<LocStringForm>(forms, [&](const LocStringForm& form) {
        if (!form.editorId.empty()) {
            entries.insert_or_assign(form.editorId, form.text);
        }
    });
}

str TextTable::get(std::string_view key) const {
    const auto it = entries.find(str { key });
    return it != entries.end() ? it->second : str { key };
}

str TextTable::format(std::string_view key, const str& arg) const {
    str text = get(key);
    if (const size_t slot = text.find("{}"); slot != str::npos) {
        text.replace(slot, 2, arg);
    }
    return text;
}

} // namespace data
