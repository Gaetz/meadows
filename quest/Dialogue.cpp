#include "quest/Dialogue.hpp"

#include <algorithm>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "gameplay/condition/Condition.hpp"
#include "gameplay/event/EventBus.hpp"

namespace quest {

namespace {

template<typename T, typename Fn>
void forEachForm(const data::FormDatabase& forms, Fn&& fn) {
    const u32 typeId = T::staticTypeInfo().id;
    for (u32 value = 1; value <= forms.count(); ++value) {
        const data::FormHandle handle { value };
        const reflect::TypeInfo* type = forms.typeOf(handle);
        const data::Form* form = forms.get(handle);
        if (type && form && type->isA(typeId)) {
            fn(*static_cast<const T*>(form));
        }
    }
}

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
    forEachForm<DialogueNodeForm>(forms, [&](const DialogueNodeForm& node) {
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
    // Advance to the NPC reply that follows this option (lowest order), if any.
    const DialogueNodeForm* reply = nullptr;
    forEachForm<DialogueNodeForm>(forms, [&](const DialogueNodeForm& node) {
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
