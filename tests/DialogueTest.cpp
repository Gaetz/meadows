#include <doctest/doctest.h>

#include <memory>

#include "data/forms/FormDatabase.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/condition/Condition.hpp"
#include "gameplay/event/EventBus.hpp"
#include "quest/Dialogue.hpp"

using core::Guid;
using namespace quest;

namespace {

const Guid kDialogue = *Guid::fromString("80000000-0000-4000-8000-000000000001");
const Guid kRoot = *Guid::fromString("80000000-0000-4000-8000-00000000000a");
const Guid kAccept = *Guid::fromString("80000000-0000-4000-8000-0000000000b1");
const Guid kBrag = *Guid::fromString("80000000-0000-4000-8000-0000000000b2");
const Guid kThanks = *Guid::fromString("80000000-0000-4000-8000-0000000000c1");
const Guid kBragCond = *Guid::fromString("80000000-0000-4000-8000-0000000000d2");

void addNode(data::FormDatabase& db, const Guid& id, const Guid& parent,
             const char* speaker, const char* text, const char* event,
             i32 order) {
    auto node = std::make_unique<DialogueNodeForm>();
    node->id = id;
    node->parent = parent;
    node->speaker = speaker;
    node->text = text;
    node->event = event;
    node->order = order;
    db.add(std::move(node), DialogueNodeForm::staticTypeInfo());
}

data::FormDatabase buildDialogueDb() {
    data::FormDatabase db;
    auto dialogue = std::make_unique<DialogueForm>();
    dialogue->id = kDialogue;
    dialogue->rootNode = kRoot;
    db.add(std::move(dialogue), DialogueForm::staticTypeInfo());

    addNode(db, kRoot, {}, "Guard", "Adventurer, will you help?", "", 0);
    addNode(db, kAccept, kRoot, "Player", "Of course.", "OnAccept", 0);
    addNode(db, kBrag, kRoot, "Player", "I am the strongest!", "", 1); // gated
    addNode(db, kThanks, kAccept, "Guard", "Thank you!", "", 0);

    // The "brag" option requires Status.Brave.
    auto cond = std::make_unique<gameplay::ConditionForm>();
    cond->id = kBragCond;
    cond->parent = kBrag;
    cond->kind = "HasTag";
    cond->tag = "Status.Brave";
    db.add(std::move(cond), gameplay::ConditionForm::staticTypeInfo());
    return db;
}

} // namespace

TEST_CASE("dialogue: chunk flow, condition-gated options, events on select") {
    const data::FormDatabase db = buildDialogueDb();
    gameplay::EventBus bus;
    gameplay::GameplayTagRegistry tags;
    tags.registerTag("Status.Brave");

    gameplay::AbilitySystem player; // no Status.Brave
    gameplay::EvalContext ctx;
    ctx.abilitySystem = &player;
    ctx.tags = &tags;

    bool accepted = false;
    bus.subscribe(gameplay::eventKind("OnAccept"),
                  [&](const gameplay::Event&) { accepted = true; });

    DialogueRunner runner { db, bus };
    REQUIRE(runner.start(kDialogue));
    REQUIRE(runner.currentLine() != nullptr);
    CHECK(runner.currentLine()->id == kRoot);

    // Only "Of course." is available; the brag is gated by Status.Brave.
    auto options = runner.options(ctx);
    REQUIRE(options.size() == 1);
    CHECK(options[0]->id == kAccept);

    // Grant the tag → the brag option appears (sorted after Accept).
    player.tags.add(*tags.find("Status.Brave"), tags);
    options = runner.options(ctx);
    REQUIRE(options.size() == 2);
    CHECK(options[0]->id == kAccept);
    CHECK(options[1]->id == kBrag);

    // Select "Of course." → fires OnAccept, advances to the NPC's thanks.
    runner.select(*db.find<DialogueNodeForm>(kAccept));
    CHECK(accepted);
    REQUIRE(runner.currentLine() != nullptr);
    CHECK(runner.currentLine()->id == kThanks);

    // No further options → the conversation can end.
    CHECK(runner.options(ctx).empty());
}

// 8.7e — the world-side-effect hook: onNodeFired fires for every entered
// NPC line AND every picked option (where the game layer applies
// takeItem/takeCount to the player inventory).
TEST_CASE("dialogue: onNodeFired reports every fired node, with its fields") {
    data::FormDatabase db = buildDialogueDb();
    // Give the accept option a hand-over: 3 of some item.
    const Guid kRation =
        *Guid::fromString("80000000-0000-4000-8000-0000000000e1");
    auto* accept =
        const_cast<DialogueNodeForm*>(db.find<DialogueNodeForm>(kAccept));
    REQUIRE(accept != nullptr);
    accept->takeItem = kRation;
    accept->takeCount = 3;

    gameplay::EventBus bus;
    DialogueRunner runner { db, bus };
    vector<Guid> fired;
    Guid takenItem;
    i32 takenCount = 0;
    runner.onNodeFired = [&](const DialogueNodeForm& node) {
        fired.push_back(node.id);
        if (node.takeItem.isValid()) {
            takenItem = node.takeItem;
            takenCount = node.takeCount;
        }
    };

    REQUIRE(runner.start(kDialogue));
    REQUIRE(fired.size() == 1); // the entered root line
    CHECK(fired[0] == kRoot);

    runner.select(*accept);
    // The picked option fires, then the NPC reply it leads to.
    REQUIRE(fired.size() == 3);
    CHECK(fired[1] == kAccept);
    CHECK(fired[2] == kThanks);
    CHECK(takenItem == kRation); // the hand-over fields reached the hook
    CHECK(takenCount == 3);
}
