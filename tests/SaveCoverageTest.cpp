#include <doctest/doctest.h>

#include <string_view>

#include "data/plugins/Record.hpp"
#include "engine/core/Hash.hpp"
#include "engine/reflect/Reflect.hpp"
#include "gameplay/save/SaveForms.hpp"
#include "gameplay/save/SaveState.hpp"

// SavedStatsForm is GENERATED from the persisted components' TypeInfos
// (savedStatsComponents) minus the exclusion table, so coverage of the
// components holds by construction. What stays worth guarding: the
// generator itself (kinds, slot defaults = component defaults), the
// exclusion table's liveness, the reverse direction (every generated
// field points at exactly one live component field, names agree on their
// kind across components), and the on-disk contract — a generated field
// carries the fnv1a(name) id its hand-written predecessor did, so saves
// written before the generation keep resolving. Pure TypeInfo work: no
// registry, no plugin, no ECS world.

namespace {

str describe(std::string_view type, std::string_view field,
             std::string_view detail) {
    return str { type } + "." + str { field } + " — " + str { detail };
}

str joinLines(const vector<str>& lines) {
    str out;
    for (const str& line : lines) {
        out += "\n    " + line;
    }
    return out;
}

const reflect::TypeInfo* findComponentType(std::string_view name) {
    for (const reflect::TypeInfo* type : gameplay::savedStatsComponentTypes()) {
        if (type->name == name) {
            return type;
        }
    }
    return nullptr;
}

// SavedStatsForm's own fields that are NOT generated: the child-record
// link to the actor's ReferenceForm.
bool isHandWrittenField(const reflect::FieldInfo& field) {
    return field.name == "parent";
}

} // namespace

TEST_CASE("save coverage: the generated SavedStatsForm carries every "
          "persisted component field with its kind and default") {
    const reflect::TypeInfo& saved = gameplay::SavedStatsForm::staticTypeInfo();
    const auto components = gameplay::savedStatsComponents();
    REQUIRE(!components.empty());
    CHECK(components.size() == gameplay::savedStatsComponentTypes().size());

    const gameplay::SavedStatsForm pristine;
    vector<str> wrong;
    size_t generated = 0;
    for (const gameplay::SavedStatsComponent& component : components) {
        REQUIRE(component.type != nullptr);
        REQUIRE(component.defaults != nullptr);
        reflect::forEachField(*component.type, [&](const reflect::FieldInfo& field) {
            if (gameplay::isSavedStatsExcluded(component.type->name, field.name)) {
                return;
            }
            const reflect::FieldInfo* mirror = saved.findField(field.id);
            if (!mirror) {
                wrong.push_back(describe(component.type->name, field.name,
                                         "not generated onto SavedStatsForm"));
                return;
            }
            if (mirror->kind != field.kind) {
                wrong.push_back(describe(component.type->name, field.name,
                                         "generated with a different FieldKind"));
                return;
            }
            if (mirror->get(&pristine) != field.get(component.defaults)) {
                wrong.push_back(describe(component.type->name, field.name,
                                         "slot default differs from the "
                                         "component's C++ default"));
                return;
            }
            ++generated;
        });
    }
    CHECK_MESSAGE(wrong.empty(),
                  "generator contract broken:" << joinLines(wrong));
    CHECK(generated > 0);
}

TEST_CASE("save coverage: the exclusion table only names live component "
          "fields absent from SavedStatsForm") {
    const reflect::TypeInfo& saved = gameplay::SavedStatsForm::staticTypeInfo();

    vector<str> stale;
    for (const gameplay::SavedStatsExclusion& entry :
         gameplay::savedStatsExclusions()) {
        CHECK_MESSAGE(!entry.reason.empty(),
                      describe(entry.type, entry.field, "exclusion without a reason"));
        CHECK(gameplay::isSavedStatsExcluded(entry.type, entry.field));
        const reflect::TypeInfo* component = findComponentType(entry.type);
        if (!component) {
            stale.push_back(describe(entry.type, entry.field,
                                     "type is not a persisted component"));
            continue;
        }
        const reflect::FieldInfo* field = component->findField(entry.field);
        if (!field) {
            stale.push_back(describe(entry.type, entry.field,
                                     "field no longer exists on the component"));
            continue;
        }
        // Another component reflecting the same name would put the field
        // back on the form — and the name-match sweep would then save and
        // restore the excluded field anyway.
        if (saved.findField(field->id)) {
            stale.push_back(describe(entry.type, entry.field,
                                     "present on SavedStatsForm after all "
                                     "(a same-named field on another "
                                     "component)"));
        }
    }
    CHECK_MESSAGE(stale.empty(),
                  "stale savedStatsExclusions entries:" << joinLines(stale));
}

TEST_CASE("save coverage: every generated field targets exactly one "
          "persisted component field") {
    const reflect::TypeInfo& saved = gameplay::SavedStatsForm::staticTypeInfo();
    const auto components = gameplay::savedStatsComponentTypes();

    vector<str> dead;
    vector<str> ambiguous;
    vector<str> kindClash;
    size_t generated = 0;
    // Own fields only: the Form base (editorId) is record identity.
    for (const reflect::FieldInfo& field : saved.fields) {
        if (isHandWrittenField(field)) {
            continue;
        }
        ++generated;
        vector<str> owners;
        for (const reflect::TypeInfo* component : components) {
            const reflect::FieldInfo* target = component->findField(field.id);
            if (!target) {
                continue;
            }
            if (target->kind != field.kind) {
                // The generator shares a slot by name and asserts the kinds
                // agree; a mismatch here means that assert was bypassed.
                kindClash.push_back(describe(component->name, target->name,
                                             "kind differs from the generated "
                                             "SavedStatsForm field"));
                continue;
            }
            owners.push_back(component->name);
        }
        if (owners.empty()) {
            dead.push_back(describe("SavedStatsForm", field.name,
                                    "no persisted component carries a field "
                                    "of that name and kind"));
        } else if (owners.size() > 1) {
            str list;
            for (const str& owner : owners) {
                list += (list.empty() ? "" : ", ") + owner;
            }
            ambiguous.push_back(describe("SavedStatsForm", field.name,
                                         "shared slot across: " + list));
        }
    }
    CHECK_MESSAGE(dead.empty(),
                  "dead SavedStatsForm fields (saved but never applied to a "
                  "component):" << joinLines(dead));
    CHECK_MESSAGE(kindClash.empty(),
                  "component fields whose kind disagrees with the generated "
                  "field of the same name:" << joinLines(kindClash));
    // Field names stay unique across the persisted components: a shared
    // slot is written by whichever component is swept last and read back
    // into every one of them.
    CHECK_MESSAGE(ambiguous.empty(),
                  "SavedStatsForm slots shared by several components:"
                      << joinLines(ambiguous));
    CHECK(generated > 0);
}

TEST_CASE("save coverage: generated fields keep the hand-written ids and "
          "kinds saves were written with") {
    const reflect::TypeInfo& saved = gameplay::SavedStatsForm::staticTypeInfo();
    CHECK(saved.name == "SavedStatsForm");
    CHECK(saved.id == core::fnv1a("SavedStatsForm"));
    REQUIRE(saved.parent != nullptr);
    CHECK(saved.parent->id == core::fnv1a("Form"));

    struct Pin {
        std::string_view name;
        reflect::FieldKind kind;
    };
    constexpr Pin kPins[] = {
        { "parent", reflect::FieldKind::Guid },
        { "health", reflect::FieldKind::F32 },
        { "level", reflect::FieldKind::F32 },
        { "followerStance", reflect::FieldKind::F32 },
        { "followerActive", reflect::FieldKind::Bool },
        { "weapon", reflect::FieldKind::Guid },
        { "bounty", reflect::FieldKind::F32 },
        { "lastRestockHours", reflect::FieldKind::F32 },
    };
    for (const Pin& pin : kPins) {
        INFO(pin.name);
        const reflect::FieldInfo* field = saved.findField(core::fnv1a(pin.name));
        REQUIRE(field != nullptr);
        CHECK(field->name == pin.name);
        CHECK(field->id == core::fnv1a(pin.name));
        CHECK(field->kind == pin.kind);
        CHECK(field->flags == reflect::None);
    }

    // The slot defaults are the values the hand-written form carried.
    const gameplay::SavedStatsForm pristine;
    CHECK(pristine.get<f32>("health") == 100.0f);
    CHECK(pristine.get<f32>("maxEssence") == 50.0f);
    CHECK(pristine.get<f32>("hunger") == 100.0f);
    CHECK(pristine.get<f32>("strength") == 6.0f);
    CHECK(pristine.get<f32>("insight") == 0.0f);
    CHECK(pristine.get<f32>("level") == 1.0f);
    CHECK(pristine.get<f32>("followerLevel") == 1.0f);
    CHECK(pristine.get<f32>("followerStance") == 0.0f);
    CHECK_FALSE(pristine.get<bool>("followerActive"));
    CHECK(pristine.get<core::Guid>("weapon") == core::Guid {});
}

TEST_CASE("save coverage: a generated field reads and writes through the "
          "reflected accessors and the record diff") {
    gameplay::SavedStatsForm form;
    const reflect::TypeInfo& saved = gameplay::SavedStatsForm::staticTypeInfo();
    const reflect::FieldInfo* health = saved.findField("health");
    REQUIRE(health != nullptr);

    // A pristine form diffs to an EMPTY record: the capture relies on it
    // to emit only what differs from spawn state.
    const core::Guid id { 0x1u, 0x2u };
    CHECK(gameplay::createRecord(form, id).fields.empty());

    CHECK(health->set(&form, reflect::Value { 42.5f }));
    CHECK(form.get<f32>("health") == 42.5f);
    CHECK(health->get(&form) == reflect::Value { 42.5f });
    // The wrong kind is refused and leaves the slot untouched.
    CHECK_FALSE(health->set(&form, reflect::Value { true }));
    CHECK(form.get<f32>("health") == 42.5f);

    form.set<f32>("followerStance", 1.0f);
    const data::Record record = gameplay::createRecord(form, id);
    CHECK(record.fields.size() == 2);
    CHECK(record.fields.at(core::fnv1a("health")) == reflect::Value { 42.5f });
    CHECK(record.fields.at(core::fnv1a("followerStance")) ==
          reflect::Value { 1.0f });

    // Copies carry their slots (the pending layer stores forms by value).
    const gameplay::SavedStatsForm copy = form;
    CHECK(copy.get<f32>("health") == 42.5f);
    const gameplay::SavedStatsForm back =
        gameplay::formFromRecord<gameplay::SavedStatsForm>(record);
    CHECK(back.get<f32>("health") == 42.5f);
    CHECK(back.get<f32>("followerStance") == 1.0f);
    CHECK(back.get<f32>("maxHealth") == 100.0f);
}
