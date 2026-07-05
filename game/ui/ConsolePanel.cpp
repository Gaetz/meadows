#include "game/ui/ConsolePanel.hpp"

#include <sstream>

#include <imgui.h>

#include "game/ui/PropertyGrid.hpp" // valueToString / valueFromString

namespace game {

namespace {

// "IronSword.damage" -> { form, type, field } through pure reflection.
struct FieldRef {
    const data::Form* form { nullptr };
    const reflect::TypeInfo* type { nullptr };
    const reflect::FieldInfo* field { nullptr };
};

FieldRef resolveFieldRef(const data::FormDatabase& forms, const str& path) {
    const size_t dot = path.rfind('.');
    if (dot == str::npos) {
        return {};
    }
    const str editorId = path.substr(0, dot);
    const str fieldName = path.substr(dot + 1);
    for (u32 i = 1; i <= forms.count(); ++i) {
        const data::FormHandle handle { i };
        const data::Form* form = forms.get(handle);
        if (!form || form->editorId != editorId) {
            continue;
        }
        const reflect::TypeInfo* type = forms.typeOf(handle);
        return { form, type, type ? type->findField(fieldName) : nullptr };
    }
    return {};
}

} // namespace

void ConsolePanel::execute(const str& line) {
    print("> " + line);
    std::istringstream in { line };
    str command;
    in >> command;
    str rest;
    std::getline(in, rest);
    if (!rest.empty() && rest.front() == ' ') {
        rest.erase(0, 1);
    }

    if (const auto it = commands.find(command); it != commands.end()) {
        print(it->second(rest));
        return;
    }
    if (command == "help") {
        print("  get <EditorId>.<field>        read any reflected field");
        print("  set <EditorId>.<field> <val>  edit it (exports as plugin)");
        print("  find <text>                   list forms matching editorId");
        print("  undo / redo                   edit history");
        for (const auto& [name, fn] : commands) {
            print("  " + name);
        }
        print("  <anything else>               runs as Lua");
        return;
    }
    if (command == "find") {
        u32 shown = 0;
        for (u32 i = 1; i <= forms.count() && shown < 50; ++i) {
            const data::FormHandle handle { i };
            const data::Form* form = forms.get(handle);
            if (form && form->editorId.find(rest) != str::npos &&
                !form->editorId.empty()) {
                print("  " + form->editorId + "  (" +
                      forms.typeOf(handle)->name + ")");
                ++shown;
            }
        }
        if (shown == 0) {
            print("  no match");
        }
        return;
    }
    if (command == "get") {
        const FieldRef ref = resolveFieldRef(forms, rest);
        if (!ref.field) {
            print("  unknown form.field: " + rest);
            return;
        }
        // Read through the session so pending edits show.
        const data::Form* view = session.view(ref.form->id);
        print("  " + valueToString(ref.field->get(view ? view : ref.form)));
        return;
    }
    if (command == "set") {
        std::istringstream args { rest };
        str path;
        args >> path;
        str valueText;
        std::getline(args, valueText);
        if (!valueText.empty() && valueText.front() == ' ') {
            valueText.erase(0, 1);
        }
        const FieldRef ref = resolveFieldRef(forms, path);
        if (!ref.field) {
            print("  unknown form.field: " + path);
            return;
        }
        const auto value = valueFromString(ref.field->kind, valueText);
        if (!value) {
            print("  cannot parse '" + valueText + "' for this field");
            return;
        }
        session.setField(ref.form->id, ref.field->id, *value);
        print("  ok (pending edit — export to persist)");
        return;
    }
    if (command == "undo") {
        session.undo();
        print("  ok");
        return;
    }
    if (command == "redo") {
        session.redo();
        print("  ok");
        return;
    }

    // Everything else is Lua on the shared VM.
    const script::RunResult result = vm.run(line);
    print(result.ok ? "  ok" : "  lua error: " + result.error);
}

void ConsolePanel::draw() {
    ImGui::Begin("Console");
    const float footer = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("log", ImVec2(0, -footer), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const str& line : lines) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (scrollToBottom) {
        ImGui::SetScrollHereY(1.0f);
        scrollToBottom = false;
    }
    ImGui::EndChild();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##input", input, sizeof(input),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (input[0] != '\0') {
            execute(input);
            input[0] = '\0';
            scrollToBottom = true;
        }
        ImGui::SetKeyboardFocusHere(-1); // keep typing
    }
    ImGui::End();
}

} // namespace game
