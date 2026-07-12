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
    return format(key, { std::string_view { arg } });
}

str TextTable::format(std::string_view key,
                      std::initializer_list<std::string_view> args) const {
    str text = get(key);
    size_t from = 0;
    for (const std::string_view arg : args) {
        const size_t slot = text.find("{}", from);
        if (slot == str::npos) {
            break; // more args than slots: the extras are dropped
        }
        text.replace(slot, 2, arg);
        from = slot + arg.size(); // an arg containing "{}" stays literal
    }
    return text;
}

} // namespace data
