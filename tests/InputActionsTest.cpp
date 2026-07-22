#include <doctest/doctest.h>

#include <filesystem>

#include "game/InputActions.hpp"
#include "game/Settings.hpp"
#include "game/scenes/OptionsController.hpp" // decideCapture (header-only)

// The action layer: default bindings, the steal-on-
// rebind rule, name round-trips, and settings.toml persistence.
// Plus the options screen's pure half: the rebind-capture
// decision (which pressed input wins, Escape cancels).

using namespace game;
using platform::Key;
using platform::MouseButton;
using platform::PadButton;

TEST_CASE("actions: the default layout is today's keyboard + a full pad") {
    ActionMap map;
    CHECK(map.binding(InputAction::Attack).mouse == MouseButton::Left);
    CHECK(map.binding(InputAction::Attack).pad == PadButton::RightTrigger);
    CHECK(map.binding(InputAction::Block).mouse == MouseButton::Right);
    CHECK(map.binding(InputAction::DrawSheathe).key == Key::R);
    CHECK(map.binding(InputAction::Sneak).key == Key::Ctrl);
    CHECK(map.binding(InputAction::SprintDodge).key == Key::Shift);
    CHECK(map.binding(InputAction::Jump).key == Key::Space);
    CHECK(map.binding(InputAction::Interact).key == Key::E);
    CHECK(map.binding(InputAction::Pause).key == Key::Escape);
    // The alternate interaction (a follower's gear) — F / LB.
    CHECK(map.binding(InputAction::InteractAlt).key == Key::F);
    CHECK(map.binding(InputAction::InteractAlt).pad ==
          PadButton::LeftShoulder);
    // Every action has at least a pad binding (full-gamepad play).
    for (u8 i = 0; i < static_cast<u8>(InputAction::Count); ++i) {
        CHECK(map.binding(static_cast<InputAction>(i)).pad !=
              PadButton::Count);
    }
}

TEST_CASE("actions: a rebind STEALS the input from its previous owner") {
    ActionMap map;
    // E moves from Interact to Jump: one owner, no double-binding.
    map.rebindKey(InputAction::Jump, Key::E);
    CHECK(map.binding(InputAction::Jump).key == Key::E);
    CHECK(map.binding(InputAction::Interact).key == Key::Count);
    // Same rule on pad buttons.
    map.rebindPad(InputAction::Attack, PadButton::A);
    CHECK(map.binding(InputAction::Attack).pad == PadButton::A);
    CHECK(map.binding(InputAction::Jump).pad == PadButton::Count);
}

TEST_CASE("actions: names round-trip (the settings.toml vocabulary)") {
    for (u8 i = 0; i < static_cast<u8>(InputAction::Count); ++i) {
        const auto action = static_cast<InputAction>(i);
        CHECK(parseAction(actionName(action)) == action);
    }
    for (u16 i = 0; i < static_cast<u16>(Key::Count); ++i) {
        const auto key = static_cast<Key>(i);
        CHECK(parseKey(keyName(key)) == key);
    }
    for (u8 i = 0; i < static_cast<u8>(PadButton::Count); ++i) {
        const auto button = static_cast<PadButton>(i);
        CHECK(parsePadButton(padButtonName(button)) == button);
    }
    CHECK(!parseAction("no-such-action").has_value());
    CHECK(!parseKey("no-such-key").has_value());
}

TEST_CASE("settings: save/load round-trips values AND bindings") {
    const auto path = std::filesystem::temp_directory_path() /
                      "meadows-settings-roundtrip.toml";
    Settings out;
    out.mouseSensitivity = 1.5f;
    out.stickSensitivity = 3.0f;
    out.invertLookY = true;
    out.stickDeadzone = 0.2f;
    out.masterVolume = 0.7f;
    out.language = "en";
    ActionMap outMap;
    outMap.rebindKey(InputAction::Jump, Key::F);
    outMap.rebindPad(InputAction::Interact, PadButton::LeftShoulder);
    REQUIRE(saveSettings(path, out, outMap));

    Settings in;
    ActionMap inMap;
    loadSettings(path, in, inMap);
    CHECK(in.mouseSensitivity == doctest::Approx(1.5f));
    CHECK(in.stickSensitivity == doctest::Approx(3.0f));
    CHECK(in.invertLookY);
    CHECK(in.stickDeadzone == doctest::Approx(0.2f));
    CHECK(in.masterVolume == doctest::Approx(0.7f));
    CHECK(in.language == "en");
    CHECK(inMap.binding(InputAction::Jump).key == Key::F);
    CHECK(inMap.binding(InputAction::Interact).pad ==
          PadButton::LeftShoulder);
    // The untouched half of a binding round-trips too (Interact kept E).
    CHECK(inMap.binding(InputAction::Interact).key == Key::E);
    // And no duplicate owner appeared anywhere for the moved inputs.
    CHECK(inMap.binding(InputAction::Jump).pad == PadButton::A);
    std::filesystem::remove(path);
}

TEST_CASE("settings: a missing file leaves the defaults standing") {
    Settings s;
    ActionMap map;
    loadSettings(std::filesystem::temp_directory_path() /
                     "meadows-settings-does-not-exist.toml",
                 s, map);
    CHECK(s.mouseSensitivity == doctest::Approx(1.0f));
    CHECK(map.binding(InputAction::Interact).key == Key::E);
}

TEST_CASE("options capture: the rebind decision table (C9.4)") {
    using Kind = CaptureResult::Kind;
    // Nothing pressed: keep waiting.
    CHECK(decideCapture(std::nullopt, std::nullopt, std::nullopt).kind ==
          Kind::None);
    // Escape cancels — even when other inputs land the same frame.
    CHECK(decideCapture(Key::Escape, std::nullopt, std::nullopt).kind ==
          Kind::Cancel);
    CHECK(decideCapture(Key::Escape, MouseButton::Left, PadButton::A)
              .kind == Kind::Cancel);
    // A key wins over a mouse button over a pad button (one device
    // slot rebinds per capture; the others keep theirs).
    const CaptureResult key =
        decideCapture(Key::F, MouseButton::Left, PadButton::A);
    CHECK(key.kind == Kind::Key);
    CHECK(key.key == Key::F);
    const CaptureResult mouse =
        decideCapture(std::nullopt, MouseButton::Middle, PadButton::A);
    CHECK(mouse.kind == Kind::Mouse);
    CHECK(mouse.mouse == MouseButton::Middle);
    // B and Start ARE capturable — the scene gates the UI pad
    // routing while a capture is armed, so they reach only the rebind.
    const CaptureResult pad =
        decideCapture(std::nullopt, std::nullopt, PadButton::B);
    CHECK(pad.kind == Kind::Pad);
    CHECK(pad.pad == PadButton::B);
    CHECK(decideCapture(std::nullopt, std::nullopt, PadButton::Start)
              .pad == PadButton::Start);
}

TEST_CASE("options capture: applying the verdict steals like any rebind") {
    // The capture path ends in the ActionMap's rebind* mutation points —
    // the steal rule holds: binding Jump's capture to E blanks Interact.
    ActionMap map;
    const CaptureResult verdict =
        decideCapture(Key::E, std::nullopt, std::nullopt);
    REQUIRE(verdict.kind == CaptureResult::Kind::Key);
    map.rebindKey(InputAction::Jump, verdict.key);
    CHECK(map.binding(InputAction::Jump).key == Key::E);
    CHECK(map.binding(InputAction::Interact).key == Key::Count);
}
