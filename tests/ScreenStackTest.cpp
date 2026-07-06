#include <doctest/doctest.h>

#include "game/ScreenStack.hpp"

// Chantier 4 B1: the pure screen-stack logic behind the RmlUi screens.

namespace {

game::ScreenStack makeStack() {
    game::ScreenStack stack;
    stack.define({ .name = "hud",
                   .document = "hud.rml",
                   .modal = false,
                   .overlay = true });
    stack.define({ .name = "inventory",
                   .document = "inventory.rml",
                   .modal = true });
    stack.define({ .name = "pause", .document = "pause.rml", .modal = true });
    return stack;
}

} // namespace

TEST_CASE("ScreenStack: overlays sit under modals in draw order") {
    game::ScreenStack stack = makeStack();
    CHECK(stack.show("hud"));
    CHECK(stack.show("inventory"));
    CHECK(stack.show("pause"));

    const auto visible = stack.visibleScreens();
    REQUIRE(visible.size() == 3);
    CHECK(visible[0]->name == "hud");
    CHECK(visible[1]->name == "inventory");
    CHECK(visible[2]->name == "pause");
    CHECK(stack.modalOpen());
    REQUIRE(stack.topModal() != nullptr);
    CHECK(stack.topModal()->name == "pause");
}

TEST_CASE("ScreenStack: escape pops modals top-down, then falls through") {
    game::ScreenStack stack = makeStack();
    stack.show("hud");
    stack.show("inventory");
    stack.show("pause");

    CHECK(stack.closeTop());
    CHECK(stack.topModal()->name == "inventory");
    CHECK(stack.closeTop());
    CHECK_FALSE(stack.modalOpen());
    CHECK_FALSE(stack.closeTop()); // nothing left: scene handles Escape

    // The HUD overlay survived the whole time.
    const auto visible = stack.visibleScreens();
    REQUIRE(visible.size() == 1);
    CHECK(visible[0]->name == "hud");
}

TEST_CASE("ScreenStack: re-show raises an open modal, close removes anywhere") {
    game::ScreenStack stack = makeStack();
    stack.show("inventory");
    stack.show("pause");
    stack.show("inventory"); // raise, no duplicate

    auto visible = stack.visibleScreens();
    REQUIRE(visible.size() == 2);
    CHECK(visible[0]->name == "pause");
    CHECK(visible[1]->name == "inventory");

    CHECK(stack.close("pause")); // remove from under the top
    visible = stack.visibleScreens();
    REQUIRE(visible.size() == 1);
    CHECK(visible[0]->name == "inventory");
}

TEST_CASE("ScreenStack: unknown names and non-screen kinds are rejected") {
    game::ScreenStack stack = makeStack();
    CHECK_FALSE(stack.show("nope"));
    CHECK_FALSE(stack.close("nope"));

    // A screen that is neither overlay nor modal cannot be shown.
    stack.define({ .name = "plain", .document = "plain.rml" });
    CHECK_FALSE(stack.show("plain"));
}

TEST_CASE("ScreenStack: later definitions win (mod layering)") {
    game::ScreenStack stack = makeStack();
    stack.define({ .name = "inventory",
                   .document = "mods/fancy-inventory.rml",
                   .modal = true });
    stack.show("inventory");
    REQUIRE(stack.topModal() != nullptr);
    CHECK(stack.topModal()->document == "mods/fancy-inventory.rml");
}
