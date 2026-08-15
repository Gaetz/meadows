#include "game/ui/CharacterStatsPanel.hpp"

#include "gameplay/stats/GameTime.hpp"
#include "gameplay/stats/EquipmentStats.hpp"

#include <algorithm>
#include <cstdio>

#include <imgui.h>

#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/combat/Combat.hpp"
#include "gameplay/stats/Afflictions.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/Drugs.hpp"
#include "gameplay/stats/Injuries.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/ResonanceDecays.hpp"
#include "gameplay/stats/StatusBuildup.hpp"
#include "gameplay/stats/Survival.hpp"

namespace game::ui {

void drawCharacterStatsPanel(ecs::Entity actor,
                             const gameplay::CharacterTickContext& ctx,
                             gameplay::GameClock& clock,
                             const gameplay::StatModifiers& equipMods,
                             CharacterStatsDemoState* demo) {
    using namespace gameplay;

    auto& core       = actor.get_mut<CoreAttributes>();
    auto& vitals     = actor.get_mut<AttributeSet>();
    auto& resonance  = actor.get_mut<Resonance>();
    auto& survival   = actor.get_mut<Survival>();
    auto& buildup    = actor.get_mut<StatusBuildup>();
    auto& injuries   = actor.get_mut<Injuries>();
    auto& combat     = actor.get_mut<CombatState>();
    auto& system     = actor.get_mut<AbilitySystem>();
    auto& resoDecays = actor.get_mut<ResonanceDecays>();
    const auto cur = [&](const char* n) { return currentValueOf(system, attr(n)); };

    const auto makeArgs = [&]() -> GameTimeTickArgs {
        return { core, vitals, system, combat, buildup, survival,
                 injuries, resonance, resoDecays, ctx.derived, ctx.tags, ctx.tuning };
    };

    ImGui::Begin("Character stats");

    if (ImGui::CollapsingHeader("Attributes (base)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat("strength",    &core.strength,    0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("constitution",&core.constitution,0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("grace",       &core.grace,       0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("dexterity",   &core.dexterity,   0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("alacrity",    &core.alacrity,    0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("perception",  &core.perception,  0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("charisma",    &core.charisma,    0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("ego",         &core.ego,         0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("insight",     &core.insight,     0.1f, 0.0f, 40.0f);
    }

    ImGui::SeparatorText("Vitals");
    const auto bar = [&](const char* label, const char* value, const char* maxField) {
        const f32 v = cur(value);
        const f32 m = cur(maxField);
        char overlay[32];
        std::snprintf(overlay, sizeof(overlay), "%.0f / %.0f", v, m);
        ImGui::ProgressBar(m > 0.0f ? v / m : 0.0f, ImVec2(-1.0f, 0.0f), overlay);
        ImGui::SameLine();
        ImGui::Text("%s", label);
    };
    bar("health",  "health",  "maxHealth");
    bar("energy",  "energy",  "maxEnergy");
    bar("essence", "essence", "maxEssence");
    {
        const f32 maxP = cur("maxPosture");
        char overlay[32];
        std::snprintf(overlay, sizeof(overlay), "%.0f / %.0f", combat.posture, maxP);
        ImGui::ProgressBar(maxP > 0.0f ? combat.posture / maxP : 0.0f,
                           ImVec2(-1.0f, 0.0f), overlay);
        ImGui::SameLine();
        ImGui::Text("posture");
    }
    if (const auto st = ctx.tags.find("State.Staggered"); st && system.tags.has(*st)) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "STAGGERED (%.1fs)", combat.staggerSeconds);
    }
    if (const auto pa = ctx.tags.find("State.Paralyzed"); pa && system.tags.has(*pa)) {
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "PARALYZED (%.1fs)", combat.paralysisSeconds);
    }
    if (const auto dead = ctx.tags.find("State.Dead"); dead && system.tags.has(*dead)) {
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "DEAD");
    }

    ImGui::SeparatorText("Resonance");
    ImGui::DragFloat("onyx (health)##pers",    &resonance.onyx,   0.5f, -100.0f, 200.0f);
    ImGui::DragFloat("amber (energy)##pers",   &resonance.amber,  0.5f, -100.0f, 200.0f);
    ImGui::DragFloat("garnet (essence)##pers", &resonance.garnet, 0.5f, -100.0f, 200.0f);

    // Breakdown from GAS active effects by source.
    f32 survAmber = 0.0f, survGarnet = 0.0f, injOnyx = 0.0f;
    f32 otherOnyx = 0.0f, otherAmber = 0.0f, otherGarnet = 0.0f;
    {
        const auto survAmberTagOpt  = ctx.tags.find("Internal.SurvivalAmber");
        const auto survGarnetTagOpt = ctx.tags.find("Internal.SurvivalGarnet");
        const auto injTagOpt        = ctx.tags.find("Injury.Active");
        const u32 kOnyx = attr("onyx"), kAmber = attr("amber"), kGarnet = attr("garnet");
        for (const auto& ae : system.activeEffects) {
            if (ae.op != ModifierOp::Add) continue;
            const bool isSurvAmber  = survAmberTagOpt  && ae.grantedTag == *survAmberTagOpt;
            const bool isSurvGarnet = survGarnetTagOpt && ae.grantedTag == *survGarnetTagOpt;
            const bool isInj        = injTagOpt        && ae.grantedTag == *injTagOpt;
            if (ae.attribute == kAmber) {
                if (isSurvAmber) survAmber += ae.magnitude;
                else otherAmber += ae.magnitude;
            } else if (ae.attribute == kGarnet) {
                if (isSurvGarnet) survGarnet += ae.magnitude;
                else otherGarnet += ae.magnitude;
            } else if (ae.attribute == kOnyx) {
                if (isInj) injOnyx += ae.magnitude;
                else otherOnyx += ae.magnitude;
            }
        }
    }

    // Raw per-channel sum and post-harmony totals.
    Resonance rawReso;
    rawReso.onyx   = resonance.onyx   + injOnyx   + otherOnyx;
    rawReso.amber  = resonance.amber  + survAmber  + otherAmber;
    rawReso.garnet = resonance.garnet + survGarnet + otherGarnet;
    const bool hBroken = harmonyBroken(system, ctx.tags);
    const Resonance postH = hBroken ? rawReso : harmonyEffective(rawReso);
    Resonance cascadeC;
    cascadeC.onyx   = postH.onyx   - rawReso.onyx;
    cascadeC.amber  = postH.amber  - rawReso.amber;
    cascadeC.garnet = postH.garnet - rawReso.garnet;

    const float labelW = 135.0f;
    const float valW   = 52.0f;
    const float barH   = 14.0f;
    const float range  = 150.0f;

    // Horizontal stacked resonance bar: pers | cascade | effects | surv | inj
    auto drawResBar = [&](const char* label,
                          f32 pers, f32 surv, f32 inj, f32 others, f32 cascade) {
        const f32   total = pers + surv + inj + others + cascade;
        const float barW  = std::max(60.0f,
                                ImGui::GetContentRegionAvail().x - labelW - valW - 8.0f);
        const float scale = (barW * 0.5f) / range;

        ImGui::Text("%-16s", label);
        ImGui::SameLine(labelW);

        const ImVec2 p  = ImGui::GetCursorScreenPos();
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, {p.x + barW, p.y + barH}, IM_COL32(35, 35, 35, 255));

        const float zx = p.x + barW * 0.5f;
        float rEdge = zx, lEdge = zx;

        auto seg = [&](float val, ImU32 col) {
            if (val > 0.005f) {
                const float w = val * scale;
                dl->AddRectFilled({rEdge, p.y + 1}, {rEdge + w, p.y + barH - 1}, col);
                rEdge += w;
            } else if (val < -0.005f) {
                const float w = (-val) * scale;
                dl->AddRectFilled({lEdge - w, p.y + 1}, {lEdge, p.y + barH - 1}, col);
                lEdge -= w;
            }
        };

        seg(pers > 0 ? pers : 0.0f, IM_COL32( 80, 140, 200, 230)); // positive pers (blue)
        seg(pers < 0 ? pers : 0.0f, IM_COL32(180,  60,  60, 230)); // negative pers (dark red)
        seg(cascade,                 IM_COL32(220, 210,  80, 230)); // harmony cascade (gold)
        seg(others,                  IM_COL32( 80, 180, 180, 230)); // other effects (teal)
        seg(surv,                    IM_COL32(200, 160,  60, 230)); // survival (amber)
        seg(inj,                     IM_COL32(200,  80,  80, 230)); // injury (red)

        dl->AddLine({zx, p.y}, {zx, p.y + barH}, IM_COL32(220, 220, 220, 180), 1.5f);
        ImGui::Dummy({barW, barH});

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextColored({0.55f, 0.75f, 1.0f,  1.0f}, "persistent   %+.1f", pers);
            if (std::abs(cascade) > 0.01f)
                ImGui::TextColored({1.0f,  0.95f, 0.35f, 1.0f}, "cascade      %+.1f", cascade);
            if (std::abs(others) > 0.01f)
                ImGui::TextColored({0.35f, 0.85f, 0.85f, 1.0f}, "effects      %+.1f", others);
            if (std::abs(surv)   > 0.01f)
                ImGui::TextColored({1.0f,  0.75f, 0.25f, 1.0f}, "survival     %+.1f", surv);
            if (std::abs(inj)    > 0.01f)
                ImGui::TextColored({1.0f,  0.4f,  0.4f,  1.0f}, "injuries     %+.1f", inj);
            ImGui::Separator();
            ImGui::Text("total        %+.1f", total);
            ImGui::EndTooltip();
        }

        ImGui::SameLine();
        ImGui::Text("%+.1f", total);
    };

    drawResBar("onyx (health)",    resonance.onyx,   0.0f,      injOnyx,    otherOnyx,   cascadeC.onyx);
    drawResBar("amber (energy)",   resonance.amber,  survAmber, 0.0f,       otherAmber,  cascadeC.amber);
    drawResBar("garnet (essence)", resonance.garnet, survGarnet, 0.0f,      otherGarnet, cascadeC.garnet);

    ImGui::TextColored({0.55f, 0.75f, 1.0f,  0.85f}, "■ pers");    ImGui::SameLine();
    ImGui::TextColored({1.0f,  0.95f, 0.35f, 0.85f}, "■ cascade"); ImGui::SameLine();
    ImGui::TextColored({0.35f, 0.85f, 0.85f, 0.85f}, "■ effects"); ImGui::SameLine();
    ImGui::TextColored({1.0f,  0.75f, 0.25f, 0.85f}, "■ surv");    ImGui::SameLine();
    ImGui::TextColored({1.0f,  0.4f,  0.4f,  0.85f}, "■ inj");

    ImGui::Text("post-harmony%s: onyx %+.1f  amber %+.1f  garnet %+.1f",
                hBroken ? " (broken)" : "", postH.onyx, postH.amber, postH.garnet);

    ImGui::SeparatorText("Survival (drag below 75 to drive resonance)");
    ImGui::DragFloat("hunger", &survival.hunger, 0.5f, 0.0f, 100.0f);
    ImGui::DragFloat("thirst", &survival.thirst, 0.5f, 0.0f, 100.0f);
    ImGui::DragFloat("sleep",  &survival.sleep,  0.5f, 0.0f, 100.0f);
    ImGui::Text("game time %.1f h   rest %.1f h", clock.gameHours(),
                combat.restSeconds / 3600.0f);

    ImGui::SeparatorText("Derived");
    ImGui::Text("str %.1f  con %.1f  gra %.1f  dex %.1f  ala %.1f  per %.1f  cha %.1f  ego %.1f  ins %.1f",
                cur("strength"), cur("constitution"), cur("grace"),
                cur("dexterity"), cur("alacrity"), cur("perception"),
                cur("charisma"), cur("ego"), cur("insight"));
    ImGui::Text("defense %.1f   armor S/B/P %.1f/%.1f/%.1f   critSens %.1f", cur("defense"),
                cur("armorSlash"), cur("armorBlunt"), cur("armorPierce"), cur("criticalSensitivity"));
    ImGui::Text("resist fire/cold/lightning %.1f/%.1f/%.1f   will %.1f",
                cur("resistFire"), cur("resistCold"), cur("resistLightning"), cur("will"));
    ImGui::Text("maxPosture %.0f   postureRegen %.1f   energyRegen %.1f",
                cur("maxPosture"), cur("postureRegen"), cur("energyRegen"));
    ImGui::Text("healthRegen %.4f/s   essenceRegen %.4f/s",
                cur("healthRegen"), cur("essenceRegen"));

    ImGui::SeparatorText("Actions");
    GameTimeTickArgs argsForDmg = makeArgs();
    const StatModifiers mods = buildCharacterMods(argsForDmg, equipMods);
    StatBlock block { core, vitals, system, combat };
    if (ImGui::Button("Slash 50")) {
        applyDamage(block, DamageEvent { { { DamageType::Slash, 50.0f } }, 25.0f },
                    ctx.tags, ctx.derived, &mods, ctx.tuning);
    }
    ImGui::SameLine();
    if (ImGui::Button("Fire 50")) {
        applyDamage(block, DamageEvent { { { DamageType::Fire, 50.0f } }, 0.0f },
                    ctx.tags, ctx.derived, &mods, ctx.tuning);
    }
    ImGui::SameLine();
    if (ImGui::Button("Posture hit")) {
        applyDamage(block, DamageEvent { {}, 40.0f }, ctx.tags, ctx.derived, &mods, ctx.tuning);
    }
    if (ImGui::Button("Wound (-15 onyx)")) {
        resonance.onyx -= 15.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Heal full")) {
        initializeActorStats(actor, ctx, equipMods);
    }
    ImGui::SameLine();
    if (ImGui::Button("Eat / drink")) {
        survival.hunger = 100.0f;
        survival.thirst = 100.0f;
    }
    if (ImGui::Button("Wait 6h (full time skip, no sleep restore)")) {
        GameTimeTickArgs gtArgs = makeArgs();
        waitGameTime(clock, gtArgs, 6.0f, equipMods);
    }
    ImGui::SameLine();
    if (ImGui::Button("Sleep 8h (rest + recovery + sleep restore)")) {
        GameTimeTickArgs gtArgs = makeArgs();
        sleepGameTime(clock, gtArgs, 8.0f, equipMods);
    }

    if (demo != nullptr) {
        ImGui::SeparatorText("Equipment (sample gear)");
        ImGui::Checkbox("Equip leather armor (+20 slash armor, +15 fire resist)",
                        &demo->armorEquipped);
        if (ImGui::Button("Attack with equipped weapon (30 slash, x1.5 strength)")) {
            const DamageResult r = applyDamage(block,
                                               weaponDamageEvent(demo->sampleWeapon, system),
                                               ctx.tags, ctx.derived, &mods, ctx.tuning);
            const f32 maxH = cur("maxHealth");
            const f32 frac = maxH > 0.0f ? r.healthDamage / maxH : 0.0f;
            const f32 onyx = currentValueOf(system, attr("onyx"));
            if (rollInjury(injuries, InjuryType::Cut, BodyPart::Torso,
                           injuryBaseChance(InjuryType::Cut, frac), onyx, demo->rng)) {
                syncInjuryEffects(injuries, system, vitals, ctx.tags);
            }
        }
    }

    ImGui::SeparatorText("Status buildup \xe2\x80\x94 fill to endurance to trigger");
    ImGui::Text("vitality %.1f%%  will %.1f%%  (vitality\xe2\x86\x92poison DoT, will\xe2\x86\x92ignition/electrocution DoT)",
                cur("vitality"), cur("will"));
    ImGui::Text("poison %.1f/%.0f  bleed %.1f/%.0f  death %.1f/%.0f",
                buildup.poison, cur("endurancePoison"),
                buildup.bleed, cur("enduranceBleed"),
                buildup.death, cur("enduranceDeath"));
    ImGui::Text("ignition %.1f/%.0f  glaciation %.1f/%.0f  electrocution %.1f/%.0f",
                buildup.ignition, cur("enduranceIgnition"),
                buildup.glaciation, cur("enduranceGlaciation"),
                buildup.electrocution, cur("enduranceElectrocution"));
    if (ImGui::Button("Poison +40")) {
        tryAddBuildup(buildup, StatusType::Poison, 40.0f, system, ctx.tags);
    }
    ImGui::SameLine();
    if (ImGui::Button("Bleed +40")) {
        tryAddBuildup(buildup, StatusType::Bleed, 40.0f, system, ctx.tags);
    }
    ImGui::SameLine();
    if (ImGui::Button("Death +40")) {
        tryAddBuildup(buildup, StatusType::Death, 40.0f, system, ctx.tags);
    }
    if (ImGui::Button("Ignition +40")) {
        tryAddBuildup(buildup, StatusType::Ignition, 40.0f, system, ctx.tags);
    }
    ImGui::SameLine();
    if (ImGui::Button("Glaciation +40")) {
        tryAddBuildup(buildup, StatusType::Glaciation, 40.0f, system, ctx.tags);
    }
    ImGui::SameLine();
    if (ImGui::Button("Electrocution +40")) {
        tryAddBuildup(buildup, StatusType::Electrocution, 40.0f, system, ctx.tags);
    }
    ImGui::SameLine();
    if (ImGui::Button("Antidote (poison -60)")) {
        buildup.poison = std::max(0.0f, buildup.poison - 60.0f);
    }
    ImGui::Text("active statuses:");
    for (const char* statusTag :
         { "Status.Poisoned", "Status.Bleeding", "Status.Mental", "Status.Diseased",
           "Status.Cursed", "Status.Dying",
           "Status.Ignited", "Status.Glaciated", "Status.Electrocuted" }) {
        if (const auto t = ctx.tags.find(statusTag); t && system.tags.has(*t)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.4f, 1.0f), "%s", statusTag);
        }
    }

    ImGui::SeparatorText("Injuries \xe2\x80\x94 heal over rest (sleep)");
    ImGui::Text("movement speed %.0f", cur("movementSpeed"));
    static const char* kInjuryTypes[] = { "Bruise", "Cut", "Fracture" };
    static const char* kInjuryParts[] = { "Head", "Torso", "Arms", "Legs" };
    static const char* kInjurySevs[]  = { "light", "major", "severe" };
    for (const Injury& inj : injuries.list) {
        ImGui::BulletText("%s %s (%s) \xe2\x80\x94 %.0fh left",
                          kInjuryTypes[static_cast<int>(inj.type)],
                          kInjuryParts[static_cast<int>(inj.part)],
                          kInjurySevs[inj.severity], inj.recoveryHoursRemaining);
    }
    if (ImGui::Button("Inflict cut (torso)")) {
        addInjury(injuries, InjuryType::Cut, BodyPart::Torso);
        syncInjuryEffects(injuries, system, vitals, ctx.tags);
    }
    ImGui::SameLine();
    if (ImGui::Button("Inflict fracture (legs)")) {
        addInjury(injuries, InjuryType::Fracture, BodyPart::Legs);
        syncInjuryEffects(injuries, system, vitals, ctx.tags);
    }

    ImGui::SeparatorText("Afflictions \xe2\x80\x94 diseases/psychoses, gated by resonance");
    {
        bool anyGameTime = false;
        for (const auto& ae : system.activeEffects) {
            if (!ae.gameTime || ae.infinite) continue;
            anyGameTime = true;
            const float remH = ae.remaining / 3600.0f;
            const char* attrLabel = (ae.attribute == attr("amber"))  ? "amber"  :
                                    (ae.attribute == attr("garnet")) ? "garnet" :
                                    (ae.attribute == attr("onyx"))   ? "onyx"   : "attr";
            ImGui::BulletText("%-8s  %+.1f  (%.1fh left)", attrLabel, ae.magnitude, remH);
        }
        if (!anyGameTime) ImGui::TextDisabled("(no active afflictions)");
    }
    if (demo != nullptr) {
        if (ImGui::Button("Inflict disease (set amber < 0 first)")) {
            const f32 amber = currentValueOf(system, attr("amber"));
            inflictEffect(vitals, system, demo->sampleDisease, amber, 1.0,
                          demo->rng, ctx.tags);
        }
        ImGui::SameLine();
        if (ImGui::Button("Inflict psychosis (set garnet < 0 first)")) {
            const f32 garnet = currentValueOf(system, attr("garnet"));
            inflictEffect(vitals, system, demo->samplePsychosis, garnet, 1.0,
                          demo->rng, ctx.tags);
        }
    }

    ImGui::SeparatorText("Drugs \xe2\x80\x94 boost, harmony break, progressive aftershock");
    ImGui::Text("harmony: %s", hBroken ? "BROKEN (channels independent)" : "intact (cascade on)");
    {
        const auto hBrokenTagOpt = ctx.tags.find("Status.HarmonyBroken");
        for (const auto& ae : system.activeEffects) {
            if (!hBrokenTagOpt || ae.grantedTag != *hBrokenTagOpt) continue;
            const float remH = ae.remaining / 3600.0f;
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.4f, 1.0f));
            ImGui::Text("  drug active: %+.1f amber  %.1fh remaining", ae.magnitude, remH);
            ImGui::PopStyleColor();
        }
    }
    if (demo != nullptr &&
        ImGui::Button("Take stimulant (+100 amber, breaks harmony, -30 aftershock)")) {
        applyEffect(vitals, system, demo->sampleDrug, ctx.tags);
    }

    ImGui::End();
}

} // namespace game::ui
