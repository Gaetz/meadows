#pragma once

#include "data/forms/Form.hpp"

// Localization Forms (horizontal pass H1). One record per string; the KEY
// is the record's editorId (e.g. "quest.intro.title"). A language pack is
// an ORDINARY plugin that patches `text` on existing records — the §5
// layering gives localization for free, and mods can localize themselves
// by shipping their own LocStringForms.
//
// FILLED (audit U4-11 brick 2): TextTable below is the cached index; the
// authoring pipeline is a CSV (editorId,text) imported by
// `cooker import-csv <csv> <toml> LocStringForm <pluginGuid>` — see
// game/data/base/text-fr.csv.

#include <string_view>
#include <unordered_map>

namespace data {

class FormDatabase;
class FormTypeRegistry;

struct LocStringForm : Form {
    str text;

    REFLECT_BEGIN(LocStringForm, Form)
        REFLECT_FIELD(text)
    REFLECT_END()
};

void registerLocFormTypes(FormTypeRegistry& registry);

// The boot-time string index: editorId (the KEY) -> text, rebuilt after
// each §5 resolve — so language packs and mod strings layer for free.
class TextTable {
public:
    void build(const FormDatabase& forms);

    // The text for `key`, or the key itself when missing — a hole shows
    // its key in game (visible and greppable), never a blank.
    str get(std::string_view key) const;

    // get(key) with the first "{}" replaced by `arg`.
    str format(std::string_view key, const str& arg) const;

    u32 size() const { return static_cast<u32>(entries.size()); }

private:
    std::unordered_map<str, str> entries;
};

} // namespace data
