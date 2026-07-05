#include <doctest/doctest.h>

#include "data/forms/AnimForms.hpp"
#include "data/forms/AudioForms.hpp"
#include "data/forms/CoreForms.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/forms/LocForms.hpp"
#include "data/forms/UiForms.hpp"
#include "data/forms/VisualForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "gameplay/actors/CharacterForms.hpp"
#include "gameplay/ai/AiForms.hpp"
#include "gameplay/interaction/FurnitureForms.hpp"
#include "world/worldspace/WorldForms.hpp"

// Horizontal pass H1: every new Form family resolves through the §5
// pipeline, the child-record convention queries cleanly, and a mod can
// patch a field or ADD a child without touching the parent.

namespace {

void registerAll(data::FormTypeRegistry& types) {
    data::registerCoreFormTypes(types);
    data::registerVisualFormTypes(types);
    data::registerAnimFormTypes(types);
    data::registerAudioFormTypes(types);
    data::registerUiFormTypes(types);
    data::registerLocFormTypes(types);
    world::registerWorldFormTypes(types);
    gameplay::registerCharacterFormTypes(types);
    gameplay::registerAiFormTypes(types);
    gameplay::registerFurnitureFormTypes(types);
}

constexpr const char* kBase = R"(
[plugin]
id = "aaaa0000-0000-4000-8000-000000000001"
name = "h1-base"

[[records]]
form = "00000001-0000-4000-8000-000000000001"
type = "MaterialForm"
new = true
[records.fields]
editorId = "MatWood"
tint = [0.8, 0.6, 0.4, 1.0]
emissive = 0.0

[[records]]
form = "00000002-0000-4000-8000-000000000001"
type = "LightForm"
new = true
[records.fields]
editorId = "Candle"
kind = "point"
color = [1.0, 0.8, 0.5]
intensity = 2.0
radius = 6.0
flicker = 0.3

[[records]]
form = "00000003-0000-4000-8000-000000000001"
type = "SoundForm"
new = true
[records.fields]
editorId = "SwordHit"
bus = "sfx"
is3d = true

[[records]]
form = "00000003-0000-4000-8000-000000000002"
type = "SoundVariantForm"
new = true
[records.fields]
editorId = "SwordHitA"
parent = "00000003-0000-4000-8000-000000000001"
weight = 2.0

[[records]]
form = "00000003-0000-4000-8000-000000000003"
type = "SoundVariantForm"
new = true
[records.fields]
editorId = "SwordHitB"
parent = "00000003-0000-4000-8000-000000000001"

[[records]]
form = "00000004-0000-4000-8000-000000000001"
type = "AnimClipForm"
new = true
[records.fields]
editorId = "AttackSlash"
rate = 1.2
loop = false

[[records]]
form = "00000004-0000-4000-8000-000000000002"
type = "AnimEventForm"
new = true
[records.fields]
editorId = "AttackSlashHit"
parent = "00000004-0000-4000-8000-000000000001"
time = 0.35
name = "Hit"

[[records]]
form = "00000005-0000-4000-8000-000000000001"
type = "ScheduleForm"
new = true
[records.fields]
editorId = "InnkeeperDay"

[[records]]
form = "00000005-0000-4000-8000-000000000002"
type = "AiPackageForm"
new = true
[records.fields]
editorId = "WorkAtBar"
kind = "work"

[[records]]
form = "00000005-0000-4000-8000-000000000003"
type = "ScheduleEntryForm"
new = true
[records.fields]
editorId = "InnkeeperWork"
parent = "00000005-0000-4000-8000-000000000001"
startHour = 8.0
endHour = 22.0
package = "00000005-0000-4000-8000-000000000002"

[[records]]
form = "00000006-0000-4000-8000-000000000001"
type = "FurnitureForm"
new = true
[records.fields]
editorId = "Bed"
category = "bed"

[[records]]
form = "00000006-0000-4000-8000-000000000002"
type = "FurniturePointForm"
new = true
[records.fields]
editorId = "BedPoint"
parent = "00000006-0000-4000-8000-000000000001"
offset = [0.0, 0.4, 0.0]
animTag = "Sleep"

[[records]]
form = "00000007-0000-4000-8000-000000000001"
type = "AppearanceForm"
new = true
[records.fields]
editorId = "VillagerLook"
skinTint = [0.9, 0.75, 0.6, 1.0]

[[records]]
form = "00000008-0000-4000-8000-000000000001"
type = "PrefabForm"
new = true
[records.fields]
editorId = "CampFire"

[[records]]
form = "00000008-0000-4000-8000-000000000002"
type = "ReferenceForm"
new = true
[records.fields]
editorId = "CampFireLog"
prefab = "00000008-0000-4000-8000-000000000001"
position = [1.0, 0.0, 0.0]

[[records]]
form = "00000009-0000-4000-8000-000000000001"
type = "UiScreenForm"
new = true
[records.fields]
editorId = "HudScreen"
screen = "hud"
document = "hud.rml"
overlay = true

[[records]]
form = "0000000a-0000-4000-8000-000000000001"
type = "LocStringForm"
new = true
[records.fields]
editorId = "quest.intro.title"
text = "The Long Road"

[[records]]
form = "0000000b-0000-4000-8000-000000000001"
type = "MarkerForm"
new = true
[records.fields]
editorId = "TavernIdleSpot"
kind = "idle"

[[records]]
form = "0000000c-0000-4000-8000-000000000001"
type = "TriggerForm"
new = true
[records.fields]
editorId = "CaveEntrance"
halfExtents = [2.0, 1.5, 0.5]
event = "OnCaveEnter"

[[records]]
form = "0000000d-0000-4000-8000-000000000001"
type = "CueForm"
new = true
[records.fields]
editorId = "HitCue"
tag = "Cue.Hit.Slash"
cameraShake = 0.2
)";

// A mod: patches one field (last-writer-wins) and ADDS a schedule entry —
// the child-record convention's whole point.
constexpr const char* kMod = R"(
[plugin]
id = "aaaa0000-0000-4000-8000-000000000002"
name = "h1-mod"

[[records]]
form = "00000002-0000-4000-8000-000000000001"
type = "LightForm"
[records.fields]
intensity = 5.0

[[records]]
form = "00000005-0000-4000-8000-000000000004"
type = "ScheduleEntryForm"
new = true
[records.fields]
editorId = "InnkeeperTavernEvening"
parent = "00000005-0000-4000-8000-000000000001"
startHour = 19.0
endHour = 23.0
)";

} // namespace

TEST_CASE("every horizontal-pass form family resolves through the plugins") {
    data::FormTypeRegistry types;
    registerAll(types);
    const auto base = data::parsePluginToml(kBase, types, "h1-base");
    REQUIRE(base.has_value());
    const auto mod = data::parsePluginToml(kMod, types, "h1-mod");
    REQUIRE(mod.has_value());

    data::FormDatabase db;
    const auto report = data::resolve({ &*base, &*mod }, types, db);
    CHECK(report.orphanPatches == 0);

    // Field-level patch across plugins: the mod's intensity wins, the
    // base's other fields survive.
    const auto* candle = data::findByEditorId<data::LightForm>(db, "Candle");
    REQUIRE(candle != nullptr);
    CHECK(candle->intensity == doctest::Approx(5.0f));
    CHECK(candle->flicker == doctest::Approx(0.3f));

    // Child-record queries.
    const auto* swordHit =
        data::findByEditorId<data::SoundForm>(db, "SwordHit");
    REQUIRE(swordHit != nullptr);
    const auto variants =
        data::collectChildren<data::SoundVariantForm>(db, swordHit->id);
    CHECK(variants.size() == 2);

    const auto* clip =
        data::findByEditorId<data::AnimClipForm>(db, "AttackSlash");
    REQUIRE(clip != nullptr);
    const auto events =
        data::collectChildren<data::AnimEventForm>(db, clip->id);
    REQUIRE(events.size() == 1);
    CHECK(events[0]->time == doctest::Approx(0.35f));
    CHECK(events[0]->name == "Hit");

    // The mod ADDED a schedule entry without touching the base records.
    const auto* schedule =
        data::findByEditorId<gameplay::ScheduleForm>(db, "InnkeeperDay");
    REQUIRE(schedule != nullptr);
    const auto entries =
        data::collectChildren<gameplay::ScheduleEntryForm>(db, schedule->id);
    CHECK(entries.size() == 2);

    // Furniture points, prefab templates, misc families.
    const auto* bed = data::findByEditorId<gameplay::FurnitureForm>(db, "Bed");
    REQUIRE(bed != nullptr);
    CHECK(data::collectChildren<gameplay::FurniturePointForm>(db, bed->id)
              .size() == 1);
    const auto* prefab =
        data::findByEditorId<world::PrefabForm>(db, "CampFire");
    REQUIRE(prefab != nullptr);
    u32 templates = 0;
    data::forEach<world::ReferenceForm>(db, [&](const world::ReferenceForm& r) {
        if (r.prefab == prefab->id) {
            ++templates;
        }
    });
    CHECK(templates == 1);
    CHECK(data::findByEditorId<data::UiScreenForm>(db, "HudScreen") != nullptr);
    CHECK(data::findByEditorId<data::LocStringForm>(db, "quest.intro.title")
              ->text == "The Long Road");
    CHECK(data::findByEditorId<world::MarkerForm>(db, "TavernIdleSpot")
              ->kind == "idle");
    CHECK(data::findByEditorId<world::TriggerForm>(db, "CaveEntrance")
              ->event == "OnCaveEnter");
    CHECK(data::findByEditorId<data::CueForm>(db, "HitCue")->tag ==
          "Cue.Hit.Slash");
}

TEST_CASE("localization: a language pack is an ordinary patch plugin") {
    data::FormTypeRegistry types;
    registerAll(types);
    const auto base = data::parsePluginToml(kBase, types, "h1-base");
    REQUIRE(base.has_value());
    constexpr const char* kFrench = R"(
[plugin]
id = "aaaa0000-0000-4000-8000-00000000000f"
name = "h1-french"

[[records]]
form = "0000000a-0000-4000-8000-000000000001"
type = "LocStringForm"
[records.fields]
text = "La Longue Route"
)";
    const auto french = data::parsePluginToml(kFrench, types, "h1-french");
    REQUIRE(french.has_value());

    data::FormDatabase db;
    data::resolve({ &*base, &*french }, types, db);
    CHECK(data::findByEditorId<data::LocStringForm>(db, "quest.intro.title")
              ->text == "La Longue Route");
}
