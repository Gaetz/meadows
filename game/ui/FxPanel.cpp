#include "game/ui/FxPanel.hpp"

#include <glm/glm.hpp>
#include <imgui.h>

#include "data/forms/VisualForms.hpp"
#include "gameplay/cue/GameplayCues.hpp"

namespace game {

void FxPanel::restart(const core::Guid& particleId) {
    sim.clear();
    age = 0.0f;
    accumulator = 0.0f;
    const auto* form =
        static_cast<const data::ParticleForm*>(session.view(particleId));
    if (form && form->burst > 0) {
        // The panel runs its OWN rate/duration emulation (with its loop
        // + pause + timescale): strip them so the sim doesn't register a
        // second, competing emitter.
        fx::EmitterParams params = gameplay::toEmitterParams(*form);
        params.rate = 0.0f;
        params.duration = 0.0f;
        sim.spawn(params, Vec3 { 0.0f }, seed++);
    }
}

void FxPanel::drawEditor(const core::Guid& particleId) {
    const auto* type =
        particleId.isValid() ? session.viewType(particleId) : nullptr;
    if (!type || type->id != data::ParticleForm::staticTypeInfo().id) {
        ImGui::TextDisabled("(select a particle effect in the Browser)");
        return;
    }
    const auto* form =
        static_cast<const data::ParticleForm*>(session.view(particleId));
    if (shown != particleId) {
        shown = particleId;
        restart(particleId);
    }

    // Controls.
    if (ImGui::Button("Restart")) {
        restart(particleId);
    }
    ImGui::SameLine();
    ImGui::Checkbox("pause", &paused);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("time", &timeScale, 0.0f, 2.0f, "%.2fx");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("zoom", &zoom, 15.0f, 200.0f, "%.0f px/m");
    ImGui::TextDisabled(
        "%u particles — %.2fs — edits apply live; blending/textures are "
        "judged in game",
        sim.count(), age);

    // Emulation: rate over duration on top of the sim's bursts. The form
    // is re-read every frame — PropertyGrid edits show immediately.
    const f32 dt = paused ? 0.0f : ImGui::GetIO().DeltaTime * timeScale;
    if (dt > 0.0f) {
        age += dt;
        const bool emitting =
            form->rate > 0.0f && (form->duration <= 0.0f
                                      ? true // continuous emitter
                                      : age < form->duration);
        if (emitting) {
            accumulator += form->rate * dt;
            fx::EmitterParams single = gameplay::toEmitterParams(*form);
            single.burst = 1;
            single.rate = 0.0f; // the panel IS the emitter loop
            single.duration = 0.0f;
            while (accumulator >= 1.0f) {
                accumulator -= 1.0f;
                sim.spawn(single, Vec3 { 0.0f }, seed++);
            }
        }
        sim.update(dt);
        // Loop: a finished one-shot (duration elapsed or burst drained)
        // replays after its longest particle dies.
        const f32 maxLife = form->lifetime * (1.0f + form->lifetimeJitter);
        const bool finished =
            sim.count() == 0 &&
            ((form->duration > 0.0f && age > form->duration + maxLife) ||
             (form->duration <= 0.0f && form->rate <= 0.0f &&
              age > maxLife));
        if (finished) {
            restart(particleId);
        }
    }

    // The stage: orthographic X (right) / Y (up), origin on the ground.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const f32 height = glm::max(200.0f, avail.y - 8.0f);
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    const ImVec2 base { corner.x + avail.x * 0.5f,
                        corner.y + height - 24.0f };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(corner, ImVec2(corner.x + avail.x, corner.y + height),
                        IM_COL32(20, 22, 26, 255));
    draw->AddLine(ImVec2(corner.x, base.y),
                  ImVec2(corner.x + avail.x, base.y),
                  IM_COL32(90, 90, 90, 160)); // the ground
    draw->PushClipRect(corner,
                       ImVec2(corner.x + avail.x, corner.y + height), true);
    const bool additive = form->blend == "additive";
    sim.forEach([&](const Vec3& position, f32 size, const Vec4& color,
                    bool) {
        Vec4 shown = color;
        if (additive) { // ImDrawList has no additive blend: brighten
            shown = glm::clamp(Vec4 { color.x * 1.5f, color.y * 1.5f,
                                      color.z * 1.5f, color.w * 0.9f },
                               Vec4 { 0.0f }, Vec4 { 1.0f });
        }
        const ImVec2 at { base.x + position.x * zoom,
                          base.y - position.y * zoom };
        draw->AddCircleFilled(
            at, glm::max(1.0f, size * zoom * 0.5f),
            IM_COL32(static_cast<int>(shown.x * 255.0f),
                     static_cast<int>(shown.y * 255.0f),
                     static_cast<int>(shown.z * 255.0f),
                     static_cast<int>(shown.w * 255.0f)));
    });
    draw->PopClipRect();
    ImGui::Dummy(ImVec2(avail.x, height));
}

} // namespace game
