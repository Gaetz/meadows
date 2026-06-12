#include "engine/ui/ImGuiLayer.hpp"

#include <SDL3/SDL.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl3.h>
#include <imgui.h>

#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"

namespace ui {

ImGuiLayer::ImGuiLayer(platform::Window& window) : window { window } {
}

ImGuiLayer::~ImGuiLayer() {
    window.setEventHook(nullptr);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

uptr<ImGuiLayer> ImGuiLayer::create(platform::Window& window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    auto* sdlWindow = static_cast<SDL_Window*>(window.nativeHandle());
    if (!ImGui_ImplSDL3_InitForOpenGL(sdlWindow, SDL_GL_GetCurrentContext()) ||
        !ImGui_ImplOpenGL3_Init("#version 460")) {
        LOG_ERROR("ImGui backend initialization failed");
        ImGui::DestroyContext();
        return nullptr;
    }

    window.setEventHook([](const void* nativeEvent) {
        ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(nativeEvent));
    });

    return uptr<ImGuiLayer> { new ImGuiLayer(window) };
}

void ImGuiLayer::beginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::render() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace ui
