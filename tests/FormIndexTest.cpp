#include <doctest/doctest.h>

#include "data/forms/AnimForms.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/core/Hash.hpp"
#include "quest/Quest.hpp"

// Secondary FormDatabase indexes: parity with
// full scans — same forms, same order — and the §5 case
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

TEST_CASE("form indexes: a ParentLink field buckets children like `parent`") {
    data::FormTypeRegistry types;
    quest::registerQuestFormTypes(types);

    const Guid questId = *Guid::fromString("bb220000-0000-4000-8000-000000000010");
    const Guid stateId = *Guid::fromString("bb220000-0000-4000-8000-000000000011");
    const Guid branchA = *Guid::fromString("bb220000-0000-4000-8000-000000000012");
    const Guid branchB = *Guid::fromString("bb220000-0000-4000-8000-000000000013");
    const Guid taskId = *Guid::fromString("bb220000-0000-4000-8000-000000000014");

    data::Plugin base;
    base.id = *Guid::fromString("bb220000-0000-4000-8000-0000000000b2");
    base.name = "quests";
    base.records.push_back(
        makeRecord(questId, quest::QuestForm::staticTypeInfo().id, true));
    auto state = makeRecord(stateId, quest::QuestStateForm::staticTypeInfo().id, true);
    state.fields[core::fnv1a("quest")] = reflect::Value { questId };
    base.records.push_back(std::move(state));
    for (const Guid& branchId : { branchA, branchB }) {
        auto branch = makeRecord(
            branchId, quest::QuestBranchForm::staticTypeInfo().id, true);
        branch.fields[core::fnv1a("state")] = reflect::Value { stateId };
        base.records.push_back(std::move(branch));
    }
    auto task = makeRecord(taskId, quest::QuestTaskForm::staticTypeInfo().id, true);
    task.fields[core::fnv1a("branch")] = reflect::Value { branchA };
    base.records.push_back(std::move(task));

    data::FormDatabase db;
    data::resolve({ &base }, types, db);
    REQUIRE(db.count() == 5);

    // `state` and `branch` are flagged reflect::ParentLink: the generic
    // parent index serves them without a private quest index.
    const auto branches =
        data::collectChildren<quest::QuestBranchForm>(db, stateId);
    REQUIRE(branches.size() == 2);
    CHECK(branches[0]->id == branchA);
    CHECK(branches[1]->id == branchB);
    const auto tasks = data::collectChildren<quest::QuestTaskForm>(db, branchA);
    REQUIRE(tasks.size() == 1);
    CHECK(tasks[0]->id == taskId);
    CHECK(data::collectChildren<quest::QuestTaskForm>(db, branchB).empty());
    // `quest` on the state is an ordinary Guid: it is NOT a parent link.
    CHECK(db.childHandles(questId).empty());
}
