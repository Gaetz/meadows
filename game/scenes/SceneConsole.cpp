#include "game/scenes/SceneConsole.hpp"

#include <imgui.h>

#include "data/plugins/EditSession.hpp"
#include "game/ui/ConsolePanel.hpp"
#include "script/Vm.hpp"

namespace game {

SceneConsole::SceneConsole() = default;
SceneConsole::~SceneConsole() = default;

ConsolePanel& SceneConsole::create(data::FormDatabase& forms,
                                   data::FormTypeRegistry& formTypes) {
    session_ = std::make_unique<data::EditSession>(forms, formTypes);
    vm_ = std::make_unique<script::Vm>();
    console_ =
        std::make_unique<ConsolePanel>(*session_, forms, formTypes, *vm_);
    return *console_;
}

void SceneConsole::reset() {
    console_.reset(); // references forms/session — before re-resolve
    vm_.reset();
    session_.reset();
    visible_ = false;
    godMode_ = false;
}

void SceneConsole::toggle(bool playMode) {
    visible_ = !visible_;
    if (visible_) {
        // Focus the input immediately: in Play the mouse is captured for
        // mouselook, so there is no cursor to click the field with.
        if (console_) {
            console_->focusInput();
        }
    } else if (playMode) {
        // Same keyboard-focus latch as enterPlayMode: closing the console
        // must not leave ImGui nav focus on a panel Play can't click away.
        ImGui::SetWindowFocus(nullptr);
    }
}

void SceneConsole::draw() const {
    if (!visible_ || !console_) {
        return;
    }
    // Quake-style: a full-width strip at the BOTTOM of the screen, the
    // log above the input field —
    // the only dev UI allowed in Play mode.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const f32 height = display.y * 0.18f; // hand-tuned
    ImGui::SetNextWindowPos(ImVec2(0.0f, display.y - height));
    ImGui::SetNextWindowSize(ImVec2(display.x, height));
    console_->draw(ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
}

} // namespace game
