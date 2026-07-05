#pragma once

#include "data/forms/Form.hpp"

// Localization Forms (horizontal pass H1). One record per string; the KEY
// is the record's editorId (e.g. "quest.intro.title"). A language pack is
// an ORDINARY plugin that patches `text` on existing records — the §5
// layering gives localization for free, and mods can localize themselves
// by shipping their own LocStringForms.
//
// HOW TO FILL (post-7/07): a `loc(db, "key")` lookup helper + a cached
// index (editorId -> handle) built after resolve; UI/dialogue/quests
// reference strings by key from day one (discipline established now).

namespace data {

class FormTypeRegistry;

struct LocStringForm : Form {
    str text;

    REFLECT_BEGIN(LocStringForm, Form)
        REFLECT_FIELD(text)
    REFLECT_END()
};

void registerLocFormTypes(FormTypeRegistry& registry);

} // namespace data
