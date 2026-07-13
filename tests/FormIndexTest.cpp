#include <doctest/doctest.h>

#include "data/forms/AnimForms.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/core/Hash.hpp"

// Secondary FormDatabase indexes (J-catalogue P1, 2026-07-13): parity with
// the historical full scans — same forms, same order — and the §5 case
// that motivated maintaining them at ADD time: a later plugin re-parents
// a child record (the resolver materializes all writes before add).

using core::Guid;

namespace {

const Guid kGraphA = *Guid::fromString("bb220000-0000-4000-8000-000000000001");
const Guid kGraphB = *Guid::fromString("bb220000-0000-4000-8000-000000000002");
const Guid kStateOne = *Guid::fromString("bb220000-0000-4000-8000-000000000003");
const Guid kStateTwo = *Guid::fromString("bb220000-0000-4000-8000-000000000004");
const Guid kClip = *Guid::fromString("bb220000-0000-4000-8000-000000000005");

data::Record makeRecord(const Guid& id, u32 typeId, bool creates) {
    data::Record record;
    record.formId = id;
    record.typeId = typeId;
    record.creates = creates;
    return record;
}

} // namespace

TEST_CASE("form indexes: scan parity, and patches re-parent children") {
    data::FormTypeRegistry types;
    data::registerAnimFormTypes(types);

    data::Plugin base;
    base.id = *Guid::fromString("bb220000-0000-4000-8000-0000000000b0");
    base.name = "base";
    base.records.push_back(makeRecord(
        kGraphA, data::AnimGraphForm::staticTypeInfo().id, true));
    base.records.push_back(makeRecord(
        kGraphB, data::AnimGraphForm::staticTypeInfo().id, true));
    base.records.push_back(
        makeRecord(kClip, data::AnimClipForm::staticTypeInfo().id, true));
    for (const Guid& stateId : { kStateOne, kStateTwo }) {
        auto state = makeRecord(
            stateId, data::AnimStateForm::staticTypeInfo().id, true);
        state.fields[core::fnv1a("parent")] = reflect::Value { kGraphA };
        state.fields[core::fnv1a("clip")] = reflect::Value { kClip };
        base.records.push_back(std::move(state));
    }

    // The §5 twist: a mod re-parents stateTwo under graph B. The resolver
    // applies the patch BEFORE the form reaches the database, so the
    // parent index must see graph B, never graph A.
    data::Plugin mod;
    mod.id = *Guid::fromString("bb220000-0000-4000-8000-0000000000b1");
    mod.name = "mod";
    auto reparent = makeRecord(
        kStateTwo, data::AnimStateForm::staticTypeInfo().id, false);
    reparent.fields[core::fnv1a("parent")] = reflect::Value { kGraphB };
    mod.records.push_back(std::move(reparent));

    data::FormDatabase db;
    data::resolve({ &base, &mod }, types, db);
    REQUIRE(db.count() == 5);

    // forEach parity: indexed iteration == the historical full scan, in
    // the same handle order.
    vector<Guid> viaIndex;
    data::forEach<data::AnimStateForm>(
        db, [&](const data::AnimStateForm& state) {
            viaIndex.push_back(state.id);
        });
    vector<Guid> viaScan;
    for (u32 i = 1; i <= db.count(); ++i) {
        const data::FormHandle handle { i };
        const reflect::TypeInfo* type = db.typeOf(handle);
        if (type &&
            type->isA(data::AnimStateForm::staticTypeInfo().id)) {
            viaScan.push_back(db.get(handle)->id);
        }
    }
    CHECK(viaIndex == viaScan);
    REQUIRE(viaIndex.size() == 2);

    // childrenOf follows the PATCHED parent.
    vector<Guid> underA;
    data::childrenOf<data::AnimStateForm>(
        db, kGraphA, [&](const data::AnimStateForm& state) {
            underA.push_back(state.id);
        });
    vector<Guid> underB;
    data::childrenOf<data::AnimStateForm>(
        db, kGraphB, [&](const data::AnimStateForm& state) {
            underB.push_back(state.id);
        });
    CHECK(underA == vector<Guid> { kStateOne });
    CHECK(underB == vector<Guid> { kStateTwo });

    // Base-type bucket: every form isA data::Form.
    CHECK(db.handlesByType(data::Form::staticTypeInfo().id).size() ==
          db.count());
    // Unknown parents/types return empty, never scan.
    CHECK(db.childHandles(kClip).empty());
    CHECK(db.handlesByType(core::fnv1a("NoSuchType")).empty());
}
