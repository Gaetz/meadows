#include "game/ui/CharacterStatsPanel.hpp"

#include <algorithm>
#include <cstdio>

#include <imgui.h>

#include "gameplay/ability/Attributes.hpp"
#include "gameplay/combat/Combat.hpp"
#include "gameplay/stats/Afflictions.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/Injuries.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/StatusBuildup.hpp"
#include "gameplay/stats/Survival.hpp"

namespace game::ui {

void drawCharacterStatsPanel(flecs::entity player,
                             const gameplay::CharacterTickContext& ctx,
                             gameplay::GameClock& clock,
                             const gameplay::StatModifiers& equipMods,
                             CharacterStatsDemoState& demo) {
    using namespace gameplay;

    auto& core        = player.get_mut<CoreAttributes>();
    auto& vitals      = player.get_mut<AttributeSet>();
    auto& resonance   = player.get_mut<Resonance>();
    auto& survival    = player.get_mut<Survival>();
    auto& buildup     = player.get_mut<StatusBuildup>();
    auto& injuries    = player.get_mut<Injuries>();
    auto& afflictions = player.get_mut<Afflictions>();
    auto& activeDrugs = player.get_mut<ActiveDrugs>();
    auto& combat      = player.get_mut<CombatState>();
    auto& system      = player.get_mut<AbilitySystem>();
    const auto cur = [&](const char* n) { return currentValueOf(system, attr(n)); };

    // Build a GameTimeTickArgs bundle from the already-fetched component refs.
    // Used by both the damage buttons (mods) and the time-advance buttons.
    const auto makeArgs = [&]() -> GameTimeTickArgs {
        return { core, vitals, system, combat, buildup, survival,
                 activeDrugs, injuries, afflictions, resonance,
                 ctx.afflictionDb, ctx.derived, ctx.tags, ctx.tuning };
    };

    ImGui::Begin("Character stats (slice)");

    if (ImGui::CollapsingHeader("Attributes (base)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat("strength", &core.strength, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("constitution", &core.constitution, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("grace", &core.grace, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("dexterity", &core.dexterity, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("alacrity", &core.alacrity, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("perception", &core.perception, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("charisma", &core.charisma, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("ego", &core.ego, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("insight", &core.insight, 0.1f, 0.0f, 40.0f);
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
    bar("health", "health", "maxHealth");
    bar("energy", "energy", "maxEnergy");
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
    ImGui::DragFloat("onyx (health)##pers",    &resonance.onyx,    0.5f, -100.0f, 200.0f);
    ImGui::DragFloat("amber (energy)##pers",   &resonance.amber,   0.5f, -100.0f, 200.0f);
    ImGui::DragFloat("garnet (essence)##pers", &resonance.garnet,  0.5f, -100.0f, 200.0f);

    // --- Breakdown histogram -------------------------------------------------
    // Compute per-source contributions (pre-harmony, pre-cascade).
    Resonance survContrib = effectiveResonance(resonance, survival, ctx.tuning);
    survContrib.onyx   -= resonance.onyx;
    survContrib.amber  -= resonance.amber;
    survContrib.garnet -= resonance.garnet;
    const f32           injOnyxC = injuryResonance(injuries);
    const Resonance     afflC    = afflictionResonance(afflictions, ctx.afflictionDb);
    const Resonance     drugC    = drugResonance(activeDrugs);
    const Resonance     aeC      = drugAftereffectResonance(activeDrugs);

    // Pre-cascade sum and post-harmony totals (needed for cascade segment below).
    Resonance fullRaw;
    fullRaw.onyx   = resonance.onyx   + survContrib.onyx   + injOnyxC + afflC.onyx   + drugC.onyx   + aeC.onyx;
    fullRaw.amber  = resonance.amber  + survContrib.amber              + afflC.amber  + drugC.amber  + aeC.amber;
    fullRaw.garnet = resonance.garnet + survContrib.garnet             + afflC.garnet + drugC.garnet + aeC.garnet;
    const bool      hBroken = harmonyBroken(system, ctx.tags);
    const Resonance postH   = hBroken ? fullRaw : harmonyEffective(fullRaw);
    // Cascade contribution per channel = what harmony adds on top of the raw sum.
    Resonance cascadeC;
    cascadeC.onyx   = postH.onyx   - fullRaw.onyx;
    cascadeC.amber  = postH.amber  - fullRaw.amber;
    cascadeC.garnet = postH.garnet - fullRaw.garnet;

    // Horizontal stacked bar: positive segs right of 0, negative segs left.
    // labelW: space reserved for the channel name on the left.
    // valW:   space for the "+xxx.x" total on the right.
    const float labelW = 135.0f;
    const float valW   = 52.0f;
    const float barH   = 14.0f;
    const float range  = 150.0f; // resonance units visible on each side of 0

    auto drawResBar = [&](const char* label,
                          f32 pers, f32 surv, f32 inj, f32 affl, f32 boost, f32 ae,
                          f32 cascade) {
        const f32   total = pers + surv + inj + affl + boost + ae + cascade;
        const float barW  = std::max(60.0f,
                                ImGui::GetContentRegionAvail().x - labelW - valW - 8.0f);
        const float scale = (barW * 0.5f) / range;

        ImGui::Text("%-16s", label);
        ImGui::SameLine(labelW);

        const ImVec2 p  = ImGui::GetCursorScreenPos();
        ImDrawList*  dl = ImGui::GetWindowDrawList();

        // Background
        dl->AddRectFilled(p, {p.x + barW, p.y + barH}, IM_COL32(35, 35, 35, 255));

        const float zx = p.x + barW * 0.5f;
        float rEdge = zx, lEdge = zx;

        // Draw one colored segment; positive → right, negative → left.
        auto seg = [&](float val, ImU32 col) {
            if (val > 0.005f) {
                float w = val * scale;
                dl->AddRectFilled({rEdge, p.y + 1}, {rEdge + w, p.y + barH - 1}, col);
                rEdge += w;
            } else if (val < -0.005f) {
                float w = (-val) * scale;
                dl->AddRectFilled({lEdge - w, p.y + 1}, {lEdge, p.y + barH - 1}, col);
                lEdge -= w;
            }
        };

        seg(pers > 0 ? pers : 0.0f, IM_COL32( 80, 140, 200, 230)); // persistent +  (blue)
        seg(pers < 0 ? pers : 0.0f, IM_COL32(180,  60,  60, 230)); // persistent -  (dark red)
        seg(cascade,                 IM_COL32(220, 210,  80, 230)); // harmony cascade (gold)
        seg(boost,                   IM_COL32( 60, 200,  80, 230)); // drug boost    (green)
        seg(surv,                    IM_COL32(200, 160,  60, 230)); // survival      (amber)
        seg(inj,                     IM_COL32(200,  80,  80, 230)); // injury        (red)
        seg(affl,                    IM_COL32( 80, 180, 180, 230)); // affliction    (teal)
        seg(ae,                      IM_COL32(160,  80, 200, 230)); // aftereffect   (purple)

        // Zero reference
        dl->AddLine({zx, p.y}, {zx, p.y + barH}, IM_COL32(220, 220, 220, 180), 1.5f);

        ImGui::Dummy({barW, barH});

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextColored({0.55f, 0.75f, 1.0f,  1.0f}, "persistent   %+.1f", pers);
            if (std::abs(cascade) > 0.01f)
                ImGui::TextColored({1.0f,  0.95f, 0.35f, 1.0f}, "cascade      %+.1f", cascade);
            if (std::abs(surv)  > 0.01f)
                ImGui::TextColored({1.0f,  0.75f, 0.25f, 1.0f}, "survival     %+.1f", surv);
            if (std::abs(inj)   > 0.01f)
                ImGui::TextColored({1.0f,  0.4f,  0.4f,  1.0f}, "injuries     %+.1f", inj);
            if (std::abs(affl)  > 0.01f)
                ImGui::TextColored({0.35f, 0.85f, 0.85f, 1.0f}, "afflictions  %+.1f", affl);
            if (std::abs(boost) > 0.01f)
                ImGui::TextColored({0.3f,  1.0f,  0.4f,  1.0f}, "drug boost   %+.1f", boost);
            if (std::abs(ae)    > 0.01f)
                ImGui::TextColored({0.75f, 0.35f, 1.0f,  1.0f}, "aftereffect  %+.1f", ae);
            ImGui::Separator();
            ImGui::Text("total        %+.1f", total);
            ImGui::EndTooltip();
        }

        ImGui::SameLine();
        ImGui::Text("%+.1f", total);
    };

    drawResBar("onyx (health)",    resonance.onyx,   survContrib.onyx,   injOnyxC, afflC.onyx,   drugC.onyx,   aeC.onyx,   cascadeC.onyx);
    drawResBar("amber (energy)",   resonance.amber,  survContrib.amber,  0.0f,     afflC.amber,  drugC.amber,  aeC.amber,  cascadeC.amber);
    drawResBar("garnet (essence)", resonance.garnet, survContrib.garnet, 0.0f,     afflC.garnet, drugC.garnet, aeC.garnet, cascadeC.garnet);

    // Legend
    ImGui::TextColored({0.55f, 0.75f, 1.0f,  0.85f}, "■ pers");    ImGui::SameLine();
    ImGui::TextColored({1.0f,  0.95f, 0.35f, 0.85f}, "■ cascade"); ImGui::SameLine();
    ImGui::TextColored({0.3f,  1.0f,  0.4f,  0.85f}, "■ boost");   ImGui::SameLine();
    ImGui::TextColored({1.0f,  0.75f, 0.25f, 0.85f}, "■ surv");    ImGui::SameLine();
    ImGui::TextColored({1.0f,  0.4f,  0.4f,  0.85f}, "■ inj");     ImGui::SameLine();
    ImGui::TextColored({0.35f, 0.85f, 0.85f, 0.85f}, "■ affl");    ImGui::SameLine();
    ImGui::TextColored({0.75f, 0.35f, 1.0f,  0.85f}, "■ afterfx");

    ImGui::Text("post-harmony%s: onyx %+.1f  amber %+.1f  garnet %+.1f",
                hBroken ? " (broken)" : "", postH.onyx, postH.amber, postH.garnet);

    ImGui::SeparatorText("Survival (drag below 75 to drive resonance)");
    ImGui::DragFloat("hunger", &survival.hunger, 0.5f, 0.0f, 100.0f);  // → amber
    ImGui::DragFloat("thirst", &survival.thirst, 0.5f, 0.0f, 100.0f);  // → amber
    ImGui::DragFloat("sleep", &survival.sleep, 0.5f, 0.0f, 100.0f);    // → garnet
    ImGui::Text("game time %.1f h   rest %.1f h", clock.gameHours(),
                combat.restSeconds / 3600.0f);

    ImGui::SeparatorText("Derived");
    // Show current attribute values (base + injury/resonance maluses).
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
    // Build mods for damage buttons — same pipeline as tickCharacter.
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
        initializeActorStats(player, ctx, equipMods);
    }
    ImGui::SameLine();
    if (ImGui::Button("Eat / drink")) {
        survival.hunger = 100.0f;
        survival.thirst = 100.0f;
    }
    if (ImGui::Button("Advance 6h (game time)")) {
        clock.gameSeconds += 6.0 * 3600.0;
        GameTimeTickArgs gtArgs = makeArgs();
        // equipMods only: buildCharacterMods inside advanceGameTime already
        // recomputes resonance/injuries/afflictions — passing full mods here
        // would double-apply them and corrupt maxHealth / attribute offsets.
        const GameTimeResult r =
            advanceGameTime(gtArgs, 6.0 * 3600.0, clock.timescale, equipMods);
        if (r.died) { /* State.Dead tag already set; UI will show DEAD */ }
    }
    ImGui::SameLine();
    if (ImGui::Button("Sleep 8h (restores sleep, accrues rest, heals injuries)")) {
        const f32 sleepBefore = survival.sleep;
        survival.sleep = 8.0f >= ctx.tuning.comfortableSleepHours
                             ? 100.0f
                             : std::min(100.0f, sleepBefore + ctx.tuning.sleepPerHour * 8.0f);
        clock.gameSeconds += 8.0 * 3600.0;
        GameTimeTickArgs gtArgs = makeArgs();
        advanceGameTime(gtArgs, 8.0 * 3600.0, clock.timescale, equipMods);
    }

    ImGui::SeparatorText("Equipment (F3)");
    ImGui::Checkbox("Equip leather armor (+20 slash armor, +15 fire resist)",
                    &demo.armorEquipped);
    if (ImGui::Button("Attack with equipped weapon (30 slash, ×1.5 strength)")) {
        const DamageResult r = applyDamage(block, weaponDamageEvent(demo.sampleWeapon, system),
                                           ctx.tags, ctx.derived, &mods, ctx.tuning);
        // Roll a cut from the hit — gated by resonance-resistance (no injury at
        // onyx ≥ 0; wound/starve the actor first to see injuries land).
        const f32 maxH = cur("maxHealth");
        const f32 frac = maxH > 0.0f ? r.healthDamage / maxH : 0.0f;
        const f32 onyx = effectiveResonance(resonance, survival, ctx.tuning).onyx +
                         injuryResonance(injuries);
        rollInjury(injuries, InjuryType::Cut, BodyPart::Torso,
                   injuryBaseChance(InjuryType::Cut, frac), onyx, demo.rng);
    }

    ImGui::SeparatorText("Status buildup (N1) — fill to endurance to trigger");
    ImGui::Text("vitality %.1f%%  will %.1f%%  (vitality→poison DoT, will→ignition/electrocution DoT)",
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

    ImGui::SeparatorText("Injuries (N2) — heal by sleeping");
    ImGui::Text("movement speed %.0f", cur("movementSpeed")); // drops with leg injury
    static const char* kInjuryTypes[] = { "Bruise", "Cut", "Fracture" };
    static const char* kInjuryParts[] = { "Head", "Torso", "Arms", "Legs" };
    static const char* kInjurySevs[] = { "light", "major", "severe" };
    for (const Injury& inj : injuries.list) {
        ImGui::BulletText("%s %s (%s) — %.0fh left", kInjuryTypes[static_cast<int>(inj.type)],
                          kInjuryParts[static_cast<int>(inj.part)],
                          kInjurySevs[inj.severity], inj.recoveryHoursRemaining);
    }
    if (ImGui::Button("Inflict cut (torso)")) {
        addInjury(injuries, InjuryType::Cut, BodyPart::Torso);
    }
    ImGui::SameLine();
    if (ImGui::Button("Inflict fracture (legs)")) {
        addInjury(injuries, InjuryType::Fracture, BodyPart::Legs);
    }

    ImGui::SeparatorText("Afflictions (N3) — diseases/psychoses, gated by amber/garnet");
    for (const ActiveAffliction& a : afflictions.list) {
        if (const AfflictionForm* def = demo.afflictionDb.find<AfflictionForm>(a.form)) {
            ImGui::BulletText("%s (%s) — %.0fh left", def->displayName.c_str(),
                              def->channel.c_str(), a.recoveryHoursRemaining);
        }
    }
    if (ImGui::Button("Inflict disease (set amber < 0 first)")) {
        if (const AfflictionForm* def = demo.afflictionDb.find<AfflictionForm>(demo.sampleDisease)) {
            const f32 amber = effectiveResonance(resonance, survival, ctx.tuning).amber +
                              afflictionResonance(afflictions, demo.afflictionDb).amber;
            inflictAffliction(afflictions, demo.sampleDisease, *def, amber, 1.0, demo.rng);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Inflict psychosis (set garnet < 0 first)")) {
        if (const AfflictionForm* def =
                demo.afflictionDb.find<AfflictionForm>(demo.samplePsychosis)) {
            const f32 garnet = effectiveResonance(resonance, survival, ctx.tuning).garnet +
                               afflictionResonance(afflictions, demo.afflictionDb).garnet;
            inflictAffliction(afflictions, demo.samplePsychosis, *def, garnet, 1.0, demo.rng);
        }
    }

    ImGui::SeparatorText("Drugs (N4) — boost, harmony break, progressive aftershock");
    ImGui::Text("harmony: %s", harmonyBroken(system, ctx.tags)
                                   ? "BROKEN (channels independent)"
                                   : "intact (cascade on)");

    // Active boosts — green countdown bar
    for (const ActiveDrug& d : activeDrugs.list) {
        const float frac = d.totalHours > 0.0f ? d.hoursRemaining / d.totalHours : 0.0f;
        ImGui::TextColored({0.3f, 1.0f, 0.4f, 1.0f}, "  [%s] %+.0f", d.channel.c_str(), d.boost);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.25f, 0.85f, 0.35f, 0.9f));
        char tbuf[32];
        std::snprintf(tbuf, sizeof(tbuf), "%.1fh / %.1fh", d.hoursRemaining, d.totalHours);
        ImGui::ProgressBar(frac, ImVec2(160.0f, 0.0f), tbuf);
        ImGui::PopStyleColor();
    }

    // Recovering aftereffects — purple decay bar
    for (const DrugAftereffect& ae : activeDrugs.aftereffects) {
        const float frac = ae.initialRemaining != 0.0f
                               ? ae.remaining / ae.initialRemaining
                               : 0.0f;
        const float recH = ae.recoveryPerHour > 0.0f ? (-ae.remaining) / ae.recoveryPerHour : 0.0f;
        ImGui::TextColored({0.75f, 0.35f, 1.0f, 1.0f}, "  [%s] %+.1f", ae.channel.c_str(), ae.remaining);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.6f, 0.25f, 0.85f, 0.9f));
        char tbuf[32];
        std::snprintf(tbuf, sizeof(tbuf), "%.1fh left", recH);
        ImGui::ProgressBar(frac, ImVec2(160.0f, 0.0f), tbuf);
        ImGui::PopStyleColor();
    }

    if (ImGui::Button("Take stimulant (+100 amber, breaks harmony, -30 aftershock)")) {
        takeDrug(activeDrugs, demo.sampleDrug, system, ctx.tags);
    }

    ImGui::End();
}

} // namespace game::ui
