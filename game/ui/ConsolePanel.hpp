#pragma once

#include "data/plugins/EditSession.hpp"
#include "script/Vm.hpp"

namespace game {

// The developer console (docs/HORIZONTAL-PASS.md) — Skyrim's console, powered
// by reflection: `set` and `get` work on EVERY form field of every type
// with zero per-type code, and anything that is not a command is Lua (the
// shared VM). Edits go through the EditSession, so console tweaks export
// to a plugin exactly like GameDB edits.
//
// HOW TO FILL: world commands (spawn/tp/tgm) need a live
// world — register extra commands from the scene that owns one via
// addCommand; keep reflection as the backbone (no per-field code, ever).
class ConsolePanel {
public:
    ConsolePanel(data::EditSession& session, const data::FormDatabase& forms,
                 script::Vm& vm)
        : session { session }, forms { forms }, vm { vm } {}

    using Command = std::function<str(const str& args)>;
    void addCommand(const str& name, Command command) {
        commands[name] = std::move(command);
    }

    // Draws the console window ("Console"). `windowFlags` lets a host
    // force a layout (the game scene docks it Quake-style: full-width
    // bottom strip, log above the input).
    void draw(int windowFlags = 0);

    // Focus the input field on the next draw — the scene calls this when the
    // console opens so the player can type without clicking (the mouse is
    // captured for mouselook in Play).
    void focusInput() { focusRequested = true; }

private:
    void execute(const str& line);
    void print(const str& line) { lines.push_back(line); }

    data::EditSession& session;
    const data::FormDatabase& forms;
    script::Vm& vm;
    std::unordered_map<str, Command> commands;

    vector<str> lines { "Console ready. `help` lists commands; anything "
                        "else runs as Lua." };
    char input[512] {};
    bool scrollToBottom { false };
    bool focusRequested { false };
};

} // namespace game
