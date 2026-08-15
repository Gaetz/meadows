#include "quest/Dialogue.hpp"

#include <algorithm>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp" // data::forEach
#include "data/forms/FormTypeRegistry.hpp"
#include "gameplay/condition/Condition.hpp"
#include "gameplay/event/EventBus.hpp"

namespace quest {

namespace {

bool isPlayer(const DialogueNodeForm& node) { return node.speaker == "Player"; }

} // namespace

void registerDialogueFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<DialogueForm>();
    registry.registerFormType<DialogueNodeForm>();
}

DialogueRunner::DialogueRunner(const data::FormDatabase& forms,
                               gameplay::EventBus& bus)
    : forms(forms), bus(bus) {}

void DialogueRunner::enter(const core::Guid& nodeId) {
    current = nodeId;
    if (const DialogueNodeForm* node = forms.find<DialogueNodeForm>(nodeId)) {
        if (!node->event.empty()) {
            bus.dispatch({ gameplay::eventKind(node->event) });
        }
        if (onNodeFired) {
            onNodeFired(*node); // world side effects (takeItem...)
        }
    } else {
        current = {};
    }
}

bool DialogueRunner::start(const core::Guid& dialogueId) {
    current = {};
    if (const DialogueForm* dialogue = forms.find<DialogueForm>(dialogueId)) {
        enter(dialogue->rootNode);
    }
    return active();
}

bool DialogueRunner::active() const { return currentLine() != nullptr; }

const DialogueNodeForm* DialogueRunner::currentLine() const {
    return current.isValid() ? forms.find<DialogueNodeForm>(current) : nullptr;
}

std::vector<const DialogueNodeForm*> DialogueRunner::options(
    const gameplay::EvalContext& context) const {
    std::vector<const DialogueNodeForm*> result;
    data::forEach<DialogueNodeForm>(forms, [&](const DialogueNodeForm& node) {
        if (node.parent == current && isPlayer(node) &&
            gameplay::conditionsPass(forms, node.id, context)) {
            result.push_back(&node);
        }
    });
    std::stable_sort(result.begin(), result.end(),
                     [](const DialogueNodeForm* a, const DialogueNodeForm* b) {
                         return a->order < b->order;
                     });
    return result;
}

void DialogueRunner::select(const DialogueNodeForm& option) {
    if (!option.event.empty()) {
        bus.dispatch({ gameplay::eventKind(option.event) });
    }
    if (onNodeFired) {
        onNodeFired(option); // world side effects (takeItem...)
    }
    // Advance to the NPC reply that follows this option (lowest order), if any.
    const DialogueNodeForm* reply = nullptr;
    data::forEach<DialogueNodeForm>(forms, [&](const DialogueNodeForm& node) {
        if (node.parent == option.id && !isPlayer(node)) {
            if (!reply || node.order < reply->order) {
                reply = &node;
            }
        }
    });
    if (reply) {
        enter(reply->id);
    } else {
        current = {}; // no reply → conversation ends
    }
}

void DialogueRunner::end() { current = {}; }

} // namespace quest
