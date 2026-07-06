#include "game/ScreenStack.hpp"

#include <algorithm>

namespace game {

void ScreenStack::define(Screen screen) {
    const auto it = std::find_if(
        screens.begin(), screens.end(),
        [&](const Screen& s) { return s.name == screen.name; });
    if (it != screens.end()) {
        *it = std::move(screen); // last definition wins (§5 layering)
    } else {
        screens.push_back(std::move(screen));
    }
}

const ScreenStack::Screen* ScreenStack::find(const str& name) const {
    const auto it = std::find_if(
        screens.begin(), screens.end(),
        [&](const Screen& s) { return s.name == name; });
    return it != screens.end() ? &*it : nullptr;
}

bool ScreenStack::show(const str& name) {
    const Screen* screen = find(name);
    if (!screen) {
        return false;
    }
    if (screen->overlay) {
        overlays.insert(name);
        return true;
    }
    if (screen->modal) {
        // Re-showing an open modal raises it to the top.
        stack.erase(std::remove(stack.begin(), stack.end(), name),
                    stack.end());
        stack.push_back(name);
        return true;
    }
    return false; // neither overlay nor modal: nothing to do with it
}

bool ScreenStack::close(const str& name) {
    if (overlays.erase(name) > 0) {
        return true;
    }
    const auto it = std::find(stack.begin(), stack.end(), name);
    if (it == stack.end()) {
        return false;
    }
    stack.erase(it);
    return true;
}

bool ScreenStack::closeTop() {
    if (stack.empty()) {
        return false;
    }
    stack.pop_back();
    return true;
}

void ScreenStack::closeAll() {
    stack.clear();
}

const ScreenStack::Screen* ScreenStack::topModal() const {
    return stack.empty() ? nullptr : find(stack.back());
}

vector<const ScreenStack::Screen*> ScreenStack::visibleScreens() const {
    vector<const Screen*> visible;
    for (const str& name : overlays) {
        if (const Screen* screen = find(name)) {
            visible.push_back(screen);
        }
    }
    for (const str& name : stack) {
        if (const Screen* screen = find(name)) {
            visible.push_back(screen);
        }
    }
    return visible;
}

} // namespace game
