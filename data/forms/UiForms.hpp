#pragma once

#include "data/forms/Form.hpp"

// UI data Forms. Game UI = RmlUi (docs/MEADOWS-PLAN.md),
// documents (.rml/.rcss) served through the plugins' `ui/` directories with
// path-level layering (a later plugin overrides a screen wholesale — the
// SkyUI/Scaleform modding model). This Form is the registry of screens: it
// names them and points at their document; a mod patches `document` to
// reskin a screen, or adds new UiScreenForms for new screens.

namespace data {

class FormTypeRegistry;

struct UiScreenForm : Form {
    str screen;   // logical name: "hud", "inventory", "dialogue"...
    str document; // path under the plugin ui/ dir, e.g. "hud.rml"
    bool modal { false };   // pauses the sim below when open
    bool overlay { false }; // HUD-style, always-on

    REFLECT_BEGIN(UiScreenForm, Form)
        REFLECT_FIELD(screen)
        REFLECT_FIELD(document)
        REFLECT_FIELD(modal)
        REFLECT_FIELD(overlay)
    REFLECT_END()
};

void registerUiFormTypes(FormTypeRegistry& registry);

} // namespace data
