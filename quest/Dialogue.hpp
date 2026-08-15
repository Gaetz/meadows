#pragma once

#include <functional>
#include <vector>

#include "data/forms/Form.hpp"

namespace data {
class FormDatabase;
class FormTypeRegistry;
}
namespace gameplay {
class EventBus;
struct EvalContext;
}

// Dialogue — NarrativePro's node graph, decomposed into Form
// records linked by id. A conversation is a tree of NPC-line nodes and Player-
// response nodes (children carry `parent`). Player options are gated by the
// shared condition evaluator; entering a node dispatches its optional
// `event` (EventBus) so quests/scripts react (e.g. an option that starts a quest).
// Single-player: no replication/party (dropped from NarrativePro).

namespace quest {

struct DialogueForm : data::Form {
    str displayName;
    core::Guid rootNode; // the first NPC node

    REFLECT_BEGIN(DialogueForm, data::Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(rootNode)
    REFLECT_END()
};

struct DialogueNodeForm : data::Form {
    core::Guid parent; // the node this follows (the root node's is unused)
    str speaker;       // "Player" for a choice; otherwise an NPC speaker id
    str text;
    str event;         // optional event name dispatched when this node is entered
    i32 order { 0 };   // sibling sort order
    // Hand items over when this node fires ("here are the rations")
    // — removed from the player through the runner's onNodeFired hook.
    // Gate the option with a HasItem condition; the removal itself does
    // not re-check.
    core::Guid takeItem;
    i32 takeCount { 1 };

    REFLECT_BEGIN(DialogueNodeForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(speaker)
        REFLECT_FIELD(text)
        REFLECT_FIELD(event)
        REFLECT_FIELD(order)
        REFLECT_FIELD(takeItem)
        REFLECT_FIELD(takeCount)
    REFLECT_END()
};

void registerDialogueFormTypes(data::FormTypeRegistry& registry);

// Runs one conversation. The caller drives it: read `currentLine`, present
// `options`, then `select` one.
class DialogueRunner {
public:
    DialogueRunner(const data::FormDatabase& forms, gameplay::EventBus& bus);

    // Called for EVERY node that fires (entered NPC line or picked
    // option), after its event dispatch. The game layer applies the
    // node's world side effects here (takeItem/takeCount -> inventory) —
    // the runner itself stays world-free and headless-testable.
    std::function<void(const DialogueNodeForm&)> onNodeFired;

    bool start(const core::Guid& dialogueId);
    bool active() const;

    // The NPC line currently being shown (nullptr if inactive).
    const DialogueNodeForm* currentLine() const;

    // The valid player responses to the current line (children that are Player
    // nodes whose conditions pass), in `order`.
    std::vector<const DialogueNodeForm*> options(
        const gameplay::EvalContext& context) const;

    // Picks an option: fires its event, then advances to the NPC reply that
    // follows it (or ends the conversation).
    void select(const DialogueNodeForm& option);

    void end(); // closes the conversation (active() becomes false)

private:
    void enter(const core::Guid& nodeId); // sets current + fires the node's event

    const data::FormDatabase& forms;
    gameplay::EventBus& bus;
    core::Guid current; // current NPC node; invalid = inactive
};

} // namespace quest
