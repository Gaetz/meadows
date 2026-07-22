#include <doctest/doctest.h>

#include "data/forms/AnimForms.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/Validate.hpp"
#include "engine/core/Hash.hpp"

// Mod lint: the reusable pass behind
// `cooker validate` — resolver facts (orphans, dependencies, conflicts)
// plus the reflection sweep for dangling guid references.

using core::Guid;

namespace {

const Guid kClip = *Guid::fromString("aa110000-0000-4000-8000-000000000001");
const Guid kMissing = *Guid::fromString("aa110000-0000-4000-8000-000000000002");
const Guid kAsset = *Guid::fromString("aa110000-0000-4000-8000-000000000003");
const Guid kGraph = *Guid::fromString("aa110000-0000-4000-8000-000000000004");
const Guid kStateOk = *Guid::fromString("aa110000-0000-4000-8000-000000000005");
const Guid kStateBad = *Guid::fromString("aa110000-0000-4000-8000-000000000006");
const Guid kOrphan = *Guid::fromString("aa110000-0000-4000-8000-000000000007");

data::Record makeRecord(const Guid& id, u32 typeId, bool creates) {
    data::Record record;
    record.formId = id;
    record.typeId = typeId;
    record.creates = creates;
    return record;
}

// base: a clip (asset declared in the VFS list), a graph and one GOOD
// state. The BAD state and the orphan patch join per test case.
data::Plugin basePlugin() {
    data::Plugin base;
    base.id = *Guid::fromString("aa110000-0000-4000-8000-0000000000b0");
    base.name = "base";
    base.assets.push_back({ kAsset, "anims/hero.gltf" });

    auto clip = makeRecord(kClip, data::AnimClipForm::staticTypeInfo().id,
                           true);
    clip.fields[core::fnv1a("asset")] = reflect::Value { kAsset };
    base.records.push_back(std::move(clip));

    auto graph = makeRecord(kGraph, data::AnimGraphForm::staticTypeInfo().id,
                            true);
    graph.fields[core::fnv1a("initialState")] = reflect::Value { kStateOk };
    base.records.push_back(std::move(graph));

    auto state = makeRecord(kStateOk,
                            data::AnimStateForm::staticTypeInfo().id, true);
    state.fields[core::fnv1a("parent")] = reflect::Value { kGraph };
    state.fields[core::fnv1a("clip")] = reflect::Value { kClip };
    base.records.push_back(std::move(state));
    return base;
}

} // namespace

TEST_CASE("validate: a clean load order passes (asset guids are known)") {
    data::FormTypeRegistry types;
    data::registerAnimFormTypes(types);
    const data::Plugin base = basePlugin();
    const auto report = data::validatePlugins({ &base }, types);
    CHECK(report.resolve.formsCreated == 3);
    CHECK(report.danglingRefs.empty()); // clip.asset resolves via the VFS list
    CHECK_FALSE(report.hasErrors());
}

TEST_CASE("validate: dangling refs and orphan patches fail; conflicts don't") {
    data::FormTypeRegistry types;
    data::registerAnimFormTypes(types);
    data::Plugin base = basePlugin();
    // A state pointing at a clip nothing creates: the dangling ref.
    auto bad = makeRecord(kStateBad, data::AnimStateForm::staticTypeInfo().id,
                          true);
    bad.fields[core::fnv1a("parent")] = reflect::Value { kGraph };
    bad.fields[core::fnv1a("clip")] = reflect::Value { kMissing };
    base.records.push_back(std::move(bad));

    data::Plugin mod;
    mod.id = *Guid::fromString("aa110000-0000-4000-8000-0000000000b1");
    mod.name = "mod";
    // Patch to a guid no plugin creates: the orphan.
    auto orphan = makeRecord(kOrphan, data::AnimClipForm::staticTypeInfo().id,
                             false);
    orphan.fields[core::fnv1a("rate")] = reflect::Value { 2.0f };
    mod.records.push_back(std::move(orphan));
    // Same-field rewrite: a CONFLICT — §5 layering, informational only.
    auto retune = makeRecord(kStateOk,
                             data::AnimStateForm::staticTypeInfo().id, false);
    retune.fields[core::fnv1a("clip")] = reflect::Value { kClip };
    mod.records.push_back(std::move(retune));

    const auto report = data::validatePlugins({ &base, &mod }, types);
    CHECK(report.resolve.orphanPatches == 1);
    REQUIRE(report.danglingRefs.size() == 1);
    CHECK(report.danglingRefs[0].form == kStateBad);
    CHECK(report.danglingRefs[0].fieldName == "clip");
    CHECK(report.danglingRefs[0].target == kMissing);
    CHECK(report.resolve.conflicts.size() == 1);
    CHECK(report.hasErrors());

    // The conflict ALONE (no orphan, no dangling) is not an error.
    data::Plugin cleanBase = basePlugin();
    data::Plugin cleanMod;
    cleanMod.id = mod.id;
    cleanMod.name = "mod";
    auto rewrite = makeRecord(
        kStateOk, data::AnimStateForm::staticTypeInfo().id, false);
    rewrite.fields[core::fnv1a("clip")] = reflect::Value { kClip };
    cleanMod.records.push_back(std::move(rewrite));
    const auto cleanReport =
        data::validatePlugins({ &cleanBase, &cleanMod }, types);
    CHECK(cleanReport.resolve.conflicts.size() == 1);
    CHECK_FALSE(cleanReport.hasErrors());
}
