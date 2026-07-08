#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/Camera3D.hpp"

namespace platform {
class Input;
class Window;
}

namespace render {

// Free-fly camera controller: a configurable mouselook trigger (hold LMB by
// default; RMB for an editor, or always-on for a spectator/Play-like feel),
// WASD to move along the view direction, E/Space up, Q/Ctrl down, Shift for
// speed boost. Pure input->camera mapping; owns no GPU state.
class FlyCamera {
public:
    Camera3D camera;

    f32 moveSpeed { 20.0f };        // m/s
    f32 fastMultiplier { 5.0f };    // while Shift is held
    f32 lookSensitivity { 0.0025f };// radians per pixel

    // What arms the mouselook capture. LeftButton (default) and RightButton
    // capture while that button is held; Always captures continuously (no
    // button), matching Play — but still yields the cursor whenever
    // `allowCapture` is false, so a hold-to-free modifier works.
    enum class LookTrigger { LeftButton, RightButton, Always };

    // `allowCapture` gates STARTING a capture (pass false while a UI layer
    // wants the mouse, e.g. ImGui::GetIO().WantCaptureMouse); an ongoing
    // button-held capture continues until release.
    void update(platform::Input& input, platform::Window& window, f32 dt,
                bool allowCapture = true,
                LookTrigger trigger = LookTrigger::LeftButton);

    bool capturing() const { return captured; }

private:
    bool captured { false };
};

} // namespace render
