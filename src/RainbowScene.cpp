#include "RainbowScene.h"
#include "Graphics/Renderer.h"
#include "Graphics/Techniques/RainbowTechnique.h"
#include <imgui.h>
#include <glm/glm.hpp>
#include <cmath>
#include <algorithm>

static constexpr float kPI = 3.14159265358979323846f;

RainbowScene::RainbowScene(graphics::techniques::RainbowTechnique* technique)
    : technique(technique)
{}

graphics::techniques::IRenderingTechnique* RainbowScene::getRenderingTechnique() const {
    return technique;
}

void RainbowScene::setRenderingTechnique(graphics::techniques::IRenderingTechnique* t) {
    technique = static_cast<graphics::techniques::RainbowTechnique*>(t);
}

void RainbowScene::onActivated(graphics::Renderer* renderer) {
    cachedRenderer = renderer;
    renderer->mainCamera.position = { 0.f, 200.f, 5.f };
    renderer->mainCamera.pitch    = glm::radians(-20.f);
    renderer->mainCamera.yaw      = 0.f;
    if (technique) technique->params.altitude = 200.f;
}

void RainbowScene::update() {
    // Sync camera Y ↔ altitude param so Q/E and the slider stay coherent
    if (cachedRenderer && technique) {
        technique->params.altitude = cachedRenderer->mainCamera.position.y;
    }
}

void RainbowScene::drawImGui() {
    // Marge gauche identique à la marge droite du diagramme
    static constexpr float kMargin = 10.f;
    ImGui::SetNextWindowPos({kMargin, kMargin}, ImGuiCond_Always);

    if (ImGui::Begin("Rainbow Scene")) {
        if (!technique) {
            ImGui::TextDisabled("No technique assigned.");
            ImGui::End();
            return;
        }

        auto& p = technique->params;

        ImGui::SeparatorText("Soleil");
        ImGui::SetNextItemWidth(200.f);
        ImGui::SliderFloat("Elevation (deg)", &p.sunElevation, 0.f, 60.f);

        ImGui::SeparatorText("Gouttelettes");
        ImGui::SetNextItemWidth(200.f);
        ImGui::SliderFloat("Rayon (mm)", &p.dropletRadius, 0.1f, 2.0f);
        ImGui::SetNextItemWidth(200.f);
        ImGui::SliderFloat("Indice de refraction", &p.refractiveIndex, 1.2f, 1.5f);
        ImGui::SameLine();
        if (ImGui::SmallButton("Eau")) p.refractiveIndex = 1.333f;

        ImGui::SeparatorText("Arcs");
        ImGui::Checkbox("Arc primaire",   &p.showPrimary);
        ImGui::Checkbox("Arc secondaire", &p.showSecondary);

        ImGui::SeparatorText("Intensite");
        ImGui::SetNextItemWidth(200.f);
        ImGui::SliderFloat("Multiplicateur", &p.intensityMult,      0.5f, 5.f,  "x%.2f");
        ImGui::SetNextItemWidth(200.f);
        ImGui::SliderFloat("Primaire",        &p.primaryIntensity,   0.f,  10.f);
        ImGui::SetNextItemWidth(200.f);
        ImGui::SliderFloat("Secondaire",      &p.secondaryIntensity, 0.f,   5.f);

        ImGui::SeparatorText("Qualite");
        ImGui::SetNextItemWidth(200.f);
        ImGui::SliderInt("Longueurs d'onde", &p.numWavelengths, 4, 32);

        ImGui::SeparatorText("Altitude");
        ImGui::SetNextItemWidth(200.f);
        if (ImGui::SliderFloat("Altitude", &p.altitude, 0.f, 5000.f, "y = %.1f")) {
            if (cachedRenderer)
                cachedRenderer->mainCamera.position.y = p.altitude;
        }
        ImGui::TextDisabled("  Q/E ou le slider pour monter/descendre");

        ImGui::SeparatorText("Terrain");
        ImGui::Checkbox("Afficher le terrain", &p.showTerrain);
        if (p.showTerrain) {
            ImGui::SetNextItemWidth(200.f);
            ImGui::SliderFloat("Hauteur max (m)",  &p.terrainHeight,      5.f,   500.f);
            ImGui::SetNextItemWidth(200.f);
            ImGui::SliderFloat("Frequence FBM",    &p.terrainFrequency,   0.001f, 0.05f, "%.4f");
            ImGui::SetNextItemWidth(200.f);
            ImGui::SliderInt("Octaves FBM",        &p.terrainOctaves,     2, 10);
            ImGui::SetNextItemWidth(200.f);
            ImGui::SliderFloat("Persistance",      &p.terrainPersistence, 0.2f,   0.8f);
            ImGui::SetNextItemWidth(200.f);
            ImGui::SliderFloat("Lumiere ambiante", &p.terrainAmbient,     0.0f,   0.5f);
        }
    }
    ImGui::End();

    drawDropletDiagram();
}

// ─────────────────────────────────────────────────────────────────────────────
// Diagramme 2-D : N rayons colorés à travers une gouttelette
//   - la taille de la goutte ∝ dropletRadius
//   - chaque rayon sortant s'élargit en éventail (±sigma = 0.008/r)
//     → petite goutte = large éventail = couleurs mélangées (blanc diffus)
//     → grande goutte = éventail étroit = couleurs séparées (arc net)
//   - tout le diagramme est incliné selon l'élévation du soleil
// ─────────────────────────────────────────────────────────────────────────────
void RainbowScene::drawDropletDiagram() {
    if (!technique) return;

    static constexpr float kMargin = 10.f;
    static constexpr float kWinW   = 420.f;

    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x - kWinW - kMargin, kMargin}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({kWinW, 400.f}, ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Gouttelette — dispersion")) { ImGui::End(); return; }

    const auto& p    = technique->params;
    float n_base     = p.refractiveIndex;
    int   N          = std::clamp(p.numWavelengths, 4, 32);
    float sunRad     = glm::radians(p.sunElevation);
    float sigma      = 0.008f / p.dropletRadius; // spread angulaire (rad) par couleur
    const float L589 = 0.589f;

    // ── Canvas ───────────────────────────────────────────────────────────────
    float canvasW = ImGui::GetContentRegionAvail().x;
    float canvasH = std::max(canvasW * 0.68f, 160.f);
    ImVec2 orig = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("dd", {canvasW, canvasH});
    ImDrawList* draw = ImGui::GetWindowDrawList();

    draw->AddRectFilled(orig, {orig.x + canvasW, orig.y + canvasH}, IM_COL32(12, 16, 28, 255));
    draw->AddRect      (orig, {orig.x + canvasW, orig.y + canvasH}, IM_COL32(55, 55, 68, 255));

    float cx = orig.x + canvasW * 0.40f;
    float cy = orig.y + canvasH * 0.50f;
    float R  = canvasH * (0.12f + 0.18f * (p.dropletRadius - 0.1f) / 1.9f);
    float inLen  = canvasW * 0.27f;
    float outLen = canvasW * 0.38f;

    // Rotation autour de (cx, cy) selon l'élévation du soleil
    // Les rayons incidents arrivent de plus en plus "d'en haut" quand le soleil monte
    float cosS = std::cos(sunRad), sinS = std::sin(sunRad);
    auto rot = [&](ImVec2 q) -> ImVec2 {
        float dx = q.x - cx, dy = q.y - cy;
        return { cx + dx*cosS - dy*sinS,
                 cy + dx*sinS + dy*cosS };
    };

    // Axe optique
    for (float x = orig.x + 4.f; x < orig.x + canvasW - 4.f; x += 9.f)
        draw->AddLine(rot({x, cy}), rot({x + 5.f, cy}),
                      IM_COL32(100, 100, 110, 50), 0.5f);

    // Gouttelette
    draw->AddCircleFilled({cx, cy}, R, IM_COL32(55, 110, 195, 38));
    draw->AddCircle      ({cx, cy}, R, IM_COL32( 95, 160, 255, 170), 64, 1.5f);

    // ── N rayons (380 nm → 700 nm) ───────────────────────────────────────────
    float cosSig = std::cos(sigma), sinSig = std::sin(sigma);

    for (int k = 0; k < N; k++) {
        float t   = float(k) / float(N - 1);
        float lam = 380.f + t * 320.f;
        float L   = lam / 1000.f;
        float n   = n_base + 6.5e-3f / (L*L) - 6.5e-3f / (L589*L589);

        float bcSq = (4.f - n*n) / 3.f;
        if (bcSq <= 0.f || bcSq >= 1.f) continue;
        float b = std::sqrt(bcSq);
        float i = std::asin(b);
        float r = std::asin(std::min(b / n, 0.9999f));

        float th1 = kPI - i;
        float th2 = 2.f*r - i;
        float th3 = 4.f*r - i - kPI;

        ImVec2 P1 = { cx + R*std::cos(th1), cy - R*std::sin(th1) };
        ImVec2 P2 = { cx + R*std::cos(th2), cy - R*std::sin(th2) };
        ImVec2 P3 = { cx + R*std::cos(th3), cy - R*std::sin(th3) };

        // Direction sortante (vecteur unitaire)
        float ddx = std::cos(th3) - std::cos(th2);
        float ddy = std::sin(th3) - std::sin(th2);
        float ddl = std::sqrt(ddx*ddx + ddy*ddy);
        ddx /= ddl;  ddy /= ddl;
        float n3x = std::cos(th3), n3y = std::sin(th3);
        float dn  = ddx*n3x + ddy*n3y;
        float tx  = ddx - dn*n3x, ty = ddy - dn*n3y;
        float tl  = std::sqrt(tx*tx + ty*ty);
        if (tl > 1e-6f) { tx /= tl; ty /= tl; }
        float ex = std::cos(i)*n3x + std::sin(i)*tx; // math y-up
        float ey = std::cos(i)*n3y + std::sin(i)*ty;

        // Couleur du rayon (violet → rouge)
        float tr = t; // 0 = violet, 1 = rouge
        int cr = (int)(20  + 200*tr);
        int cg = (int)(20  + 180*std::sin(tr*kPI));
        int cb = (int)(200 - 180*tr);
        ImU32 col     = IM_COL32(cr, cg, cb, 200);
        ImU32 colDim  = IM_COL32(cr, cg, cb,  45); // bord du fan (semi-transparent)

        // ── Chemin entrant + interne ──────────────────────────────────────────
        ImVec2 inStart = { P1.x - inLen, P1.y };
        draw->AddLine(rot(inStart), rot(P1), col, 1.4f);
        draw->AddLine(rot(P1),      rot(P2), col, 1.4f);
        draw->AddLine(rot(P2),      rot(P3), col, 1.4f);

        // ── Rayon sortant : rayon central + deux bords à ±sigma ──────────────
        // Direction en coords écran (y vers le bas) : (ex, -ey)
        float dsx = ex, dsy = -ey;

        // Rotation de dsx/dsy par ±sigma en coords écran
        // +sigma : rotation CCW en écran (vers le "haut")
        float dp_x = dsx*cosSig - dsy*sinSig;
        float dp_y = dsx*sinSig + dsy*cosSig;
        // -sigma : rotation CW en écran (vers le "bas")
        float dm_x = dsx*cosSig + dsy*sinSig;
        float dm_y = -dsx*sinSig + dsy*cosSig;

        ImVec2 exitC = { P3.x + dsx  * outLen, P3.y + dsy  * outLen };
        ImVec2 exitP = { P3.x + dp_x * outLen, P3.y + dp_y * outLen };
        ImVec2 exitM = { P3.x + dm_x * outLen, P3.y + dm_y * outLen };

        // Triangle de dispersion (montre l'étalement angulaire)
        draw->AddTriangleFilled(rot(P3), rot(exitP), rot(exitM), colDim);
        // Rayon central (plein)
        draw->AddLine(rot(P3), rot(exitC), col, 1.6f);
    }

    // ── Légende ──────────────────────────────────────────────────────────────
    ImGui::Spacing();
    float sigmaDeg = glm::degrees(sigma);
    ImGui::Text("Etalement angulaire : +/-%.1f deg  (rayon %.2f mm)",
                sigmaDeg, p.dropletRadius);
    ImGui::TextDisabled("Grande goutte -> eventail etroit -> couleurs separees (arc net)");
    ImGui::TextDisabled("Petite goutte -> eventail large  -> couleurs melangees (blanc diffus)");

    ImGui::End();
}
