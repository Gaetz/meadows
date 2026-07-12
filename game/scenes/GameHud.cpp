#include "game/scenes/GameHud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <glm/glm.hpp>

#include "data/forms/CoreForms.hpp" // data::ActorForm, MiscItemForm
#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp" // data::forEach
#include "data/forms/LocForms.hpp"  // TextTable (C9.5)
#include "engine/physics/Physics.hpp"
#include "engine/render/FlyCamera.hpp"
#include "engine/ui/UiSystem.hpp"
#include "game/Barter.hpp" // barterPrice
#include "game/ScreenStack.hpp"
#include "game/scenes/InteractionController.hpp"
#include "game/scenes/NpcDirector.hpp" // Npc (nameplates)
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/inventory/Inventory.hpp"
#include "gameplay/stats/Damage.hpp" // gameplay::CombatState
#include "gameplay/stats/EquipmentStats.hpp"
#include "gameplay/stats/GameClock.hpp"
#include "quest/Dialogue.hpp"
#include "quest/Quest.hpp"
#include "world/scene/Components.hpp"

namespace game {

void GameHud::updateHudModel(const HudContext& ctx) {
    if (!ctx.uiCreated) {
        return;
    }
    if (ctx.playerEntity.is_alive() &&
        ctx.playerEntity.has<gameplay::AbilitySystem>() &&
        ctx.playerEntity.has<gameplay::AttributeSet>()) {
        const auto& sys = ctx.playerEntity.get<gameplay::AbilitySystem>();
        const auto& vitals = ctx.playerEntity.get<gameplay::AttributeSet>();
        // Dev design 2026-07-12 — each bar tells three truths at once:
        //   outer width  = the THEORETICAL max (BaseValue; 1000 points =
        //                  the whole half-screen container),
        //   fill         = the current value (background = what's lost),
        //   resonance    = a DARKER slice for capacity the malus locked
        //                  ([effective max .. base max]), a LIGHTER fill
        //                  past the base max when a bonus extends it.
        const auto pushBar = [&](const str& prefix, f32 current,
                                 f32 baseMax, f32 effMax) {
            const f32 rendered =
                glm::max(glm::max(baseMax, effMax), 1.0f);
            const auto pct = [&](f32 v) {
                return glm::clamp(100.0f * v / rendered, 0.0f, 100.0f);
            };
            // hudStatPointsScale points = 1% of the (half-screen)
            // container — 5 -> 500 points = full width (dev feel pass
            // 2026-07-12: bars doubled in length; R7: moddable).
            ctx.ui.setNumber(
                "hud", prefix + "BarPct",
                glm::clamp(rendered / ctx.hudStatPointsScale, 2.0f,
                           100.0f));
            const f32 fill = pct(glm::min(current, baseMax));
            ctx.ui.setNumber("hud", prefix + "FillPct", fill);
            ctx.ui.setNumber("hud", prefix + "BonusPct",
                             pct(glm::max(current - baseMax, 0.0f)));
            ctx.ui.setNumber("hud", prefix + "MalusLeft", pct(effMax));
            ctx.ui.setNumber("hud", prefix + "MalusPct",
                             pct(glm::max(baseMax - effMax, 0.0f)));
        };
        // The resonance % is the ONLY thing separating the effective max
        // from the theoretical one (the derived maxima read BASE
        // attributes; resonance multiplies them by 1 + r/100 — §2). The
        // HUD does NOT re-derive that rule (review 7c): the character
        // tick publishes the pre-resonance maxima (its Phase-A
        // recompute) under the theoreticalMax* overlay ids — read them.
        // Fallback = the effective max (an actor that never ticked has
        // no published value; no resonance slice then).
        const auto theoretical = [&](const char* id, f32 effMax) {
            const f32 value = gameplay::currentValueOf(sys,
                                                       gameplay::attr(id));
            return value > 0.0f ? value : effMax;
        };
        const f32 maxHealth =
            gameplay::currentValueOf(sys, gameplay::attr("maxHealth"));
        const f32 maxEnergy =
            gameplay::currentValueOf(sys, gameplay::attr("maxEnergy"));
        const f32 maxEssence =
            gameplay::currentValueOf(sys, gameplay::attr("maxEssence"));
        pushBar("health", vitals.health,
                theoretical("theoreticalMaxHealth", maxHealth), maxHealth);
        pushBar("energy", vitals.energy,
                theoretical("theoreticalMaxEnergy", maxEnergy), maxEnergy);
        pushBar("essence", vitals.essence,
                theoretical("theoreticalMaxEssence", maxEssence),
                maxEssence);
        // Posture is a DERIVED stat (no BaseValue of its own): the bar
        // scales with the current max, no resonance slices.
        const f32 maxPosture =
            gameplay::currentValueOf(sys, gameplay::attr("maxPosture"));
        f32 posture = maxPosture;
        if (ctx.playerEntity.has<gameplay::CombatState>()) {
            posture = ctx.playerEntity.get<gameplay::CombatState>().posture;
        }
        pushBar("posture", posture, maxPosture, maxPosture);
        const auto text = [](f32 value, f32 max) {
            return std::to_string(static_cast<i32>(value + 0.5f)) + " / " +
                   std::to_string(static_cast<i32>(max + 0.5f));
        };
        ctx.ui.setString("hud", "healthText",
                         text(vitals.health, maxHealth));
        ctx.ui.setString("hud", "energyText",
                         text(vitals.energy, maxEnergy));
        ctx.ui.setString("hud", "essenceText",
                         text(vitals.essence, maxEssence));
        ctx.ui.setString("hud", "postureText", text(posture, maxPosture));
    }
    // A7+: the bow-draw gauge, bottom center while drawing.
    ctx.ui.setBool("hud", "chargeVisible", ctx.bowCharge >= 0.0f);
    ctx.ui.setNumber("hud", "chargePct",
                     glm::clamp(ctx.bowCharge, 0.0f, 1.0f) * 100.0f);
    const f64 hours = std::fmod(ctx.gameClock.gameHours(), 24.0);
    const i32 hh = static_cast<i32>(hours);
    const i32 mm = static_cast<i32>((hours - hh) * 60.0);
    char clock[8];
    std::snprintf(clock, sizeof(clock), "%02d:%02d", hh, mm);
    ctx.ui.setString("hud", "clock", clock);
    // The interaction prompt + talk line (migrated from the ImGui overlay).
    const bool promptOn = ctx.playMode && ctx.interaction.promptVisible();
    ctx.ui.setBool("hud", "promptVisible", promptOn);
    ctx.ui.setString("hud", "prompt",
                     promptOn ? ctx.interaction.promptLabel() : str {});
    const bool talkOn = ctx.interaction.talkVisible();
    ctx.ui.setBool("hud", "talkVisible", talkOn);
    ctx.ui.setString("hud", "talk",
                     talkOn ? ctx.interaction.talkLine() : str {});
    updateNameplates(ctx); // B7
}

void GameHud::updateNameplates(const HudContext& ctx) {
    vector<::ui::UiRow> plates;
    if (ctx.playMode && ctx.player && ctx.uiCreated) {
        const f32 width = ctx.screenWidth;
        const f32 height = ctx.screenHeight;
        const Mat4 viewProj = ctx.flyCamera.camera.viewProj(
            height > 0.0f ? width / height : 1.0f);
        for (const auto& npcPtr : ctx.npcs) {
            const Npc& npc = *npcPtr;
            if (npc.dead || !npc.entity.is_alive() ||
                !npc.entity.has<gameplay::AbilitySystem>()) {
                continue;
            }
            const Vec3 position =
                npc.entity.get<world::Transform>().position;
            const Vec3 to = position - ctx.player->position();
            if (glm::dot(to, to) > 15.0f * 15.0f) {
                continue;
            }
            const auto& sys = npc.entity.get<gameplay::AbilitySystem>();
            const f32 health =
                gameplay::currentValueOf(sys, gameplay::attr("health"));
            const f32 maxHealth =
                gameplay::currentValueOf(sys, gameplay::attr("maxHealth"));
            // Nameplates single out threats and the wounded (SkyUI-style
            // restraint: a healthy villager stays unlabelled).
            if (!npc.hostile && health >= maxHealth - 0.5f) {
                continue;
            }
            const Vec4 clip =
                viewProj * Vec4 { position + Vec3 { 0.0f, 2.15f, 0.0f },
                                  1.0f };
            if (clip.w <= 0.1f) {
                continue; // behind the camera
            }
            const f32 px = (clip.x / clip.w * 0.5f + 0.5f) * width;
            const f32 py =
                (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * height;
            ::ui::UiRow plate;
            plate.id = std::to_string(npc.entity.id());
            str name = "?";
            if (npc.entity.has<world::RefId>()) {
                const auto& ref = npc.entity.get<world::RefId>();
                if (const reflect::TypeInfo* type =
                        ctx.forms.typeOf(ref.base);
                    type &&
                    type->isA(data::ActorForm::staticTypeInfo().id)) {
                    name = static_cast<const data::ActorForm*>(
                               ctx.forms.get(ref.base))
                               ->displayName;
                }
            }
            plate.c0 = name;
            plate.c1 = std::to_string(static_cast<i32>(glm::clamp(
                100.0f * health / glm::max(maxHealth, 1.0f), 0.0f,
                100.0f)));
            plate.c2 = std::to_string(static_cast<i32>(px - 60.0f));
            plate.c3 = std::to_string(static_cast<i32>(py));
            // Dev design 2026-07-12: the second small bar — POISE.
            f32 posturePct = 100.0f;
            if (npc.entity.has<gameplay::CombatState>()) {
                const f32 maxPosture = gameplay::currentValueOf(
                    sys, gameplay::attr("maxPosture"));
                posturePct = glm::clamp(
                    100.0f *
                        npc.entity.get<gameplay::CombatState>().posture /
                        glm::max(maxPosture, 1.0f),
                    0.0f, 100.0f);
            }
            plate.c4 = std::to_string(static_cast<i32>(posturePct));
            plate.tag = npc.hostile ? "hostile" : "";
            plates.push_back(std::move(plate));
        }
    }
    ctx.ui.setRows("hud", std::move(plates));
}

void GameHud::pushItemModels(const HudContext& ctx) {
    if (!ctx.uiCreated) {
        return;
    }
    static const gameplay::Inventory kEmptyBag;
    const gameplay::Inventory* bag = &kEmptyBag;
    const gameplay::Equipment* equipment = nullptr;
    if (ctx.playerEntity.is_alive()) {
        if (ctx.playerEntity.has<gameplay::Inventory>()) {
            bag = &ctx.playerEntity.get<gameplay::Inventory>();
        }
        if (ctx.playerEntity.has<gameplay::Equipment>()) {
            equipment = &ctx.playerEntity.get<gameplay::Equipment>();
        }
    }
    invView.build(ctx.forms, *bag, equipment);

    // In barter mode the value column shows the PRICE at the relevant
    // multiplier (sell on the player side, buy on the vendor side).
    const auto pushRows = [&ctx](const InventoryView& view,
                                 const str& model, f32 priceMult) {
        vector<::ui::UiRow> rows;
        rows.reserve(view.rows().size());
        char buffer[32];
        for (const InventoryView::Row& row : view.rows()) {
            ::ui::UiRow out;
            out.id = row.id.toString();
            out.c0 = row.count > 1
                         ? row.name + "  x" + std::to_string(row.count)
                         : row.name;
            std::snprintf(buffer, sizeof(buffer), "%.1f", row.weight);
            out.c1 = buffer;
            out.c2 = std::to_string(
                priceMult > 0.0f ? barterPrice(row.value, priceMult)
                                 : row.value);
            out.c3 = row.power > 0.0f
                         ? std::to_string(
                               static_cast<i32>(row.power + 0.5f))
                         : str { "-" };
            out.selected = row.id == view.selected();
            out.tag = row.equipped ? "equipped" : "";
            rows.push_back(std::move(out));
        }
        ctx.ui.setRows(model, std::move(rows));
    };
    pushRows(invView, "inventory",
             ctx.barterMode ? ctx.vendorSellMult : 0.0f);

    // C3: weight / max + the encumbrance category. C9.5: every label the
    // C++ side formats is a LocStringForm key (languages layer, §5); the
    // category label reuses the stable encumbranceLabel() vocabulary.
    char number[32];
    std::snprintf(number, sizeof(number), "%.1f", invView.totalWeight());
    char maxText[32];
    f32 maxEncumbrance = 0.0f;
    if (ctx.playerEntity.is_alive()) {
        maxEncumbrance = gameplay::currentValueOf(
            ctx.playerEntity.get<gameplay::AbilitySystem>(),
            gameplay::attr("maxEncumbrance"));
    }
    std::snprintf(maxText, sizeof(maxText), "%.0f", maxEncumbrance);
    const str category = ctx.texts.get(
        str { "ui.hud.encumbrance." } +
        gameplay::encumbranceLabel(gameplay::encumbranceCategory(
            invView.totalWeight(), maxEncumbrance)));
    ctx.ui.setString("inventory", "weightText",
                     ctx.texts.format("ui.hud.carriedWeight",
                                      { number, maxText, category }));

    const InventoryView::Row* selected = invView.selectedRow();
    ctx.ui.setBool("inventory", "hasSelection", selected != nullptr);
    if (selected) {
        ctx.ui.setString("inventory", "detailName", selected->name);
        char weight[32];
        std::snprintf(weight, sizeof(weight), "%.1f", selected->weight);
        const str power =
            selected->power > 0.0f
                ? ctx.texts.format(
                      "ui.inventory.detailPower",
                      std::to_string(
                          static_cast<i32>(selected->power + 0.5f)))
                : str {};
        ctx.ui.setString(
            "inventory", "detailInfo",
            ctx.texts.format(
                "ui.inventory.detail",
                { weight, std::to_string(selected->value), power }));
        ctx.ui.setBool("inventory", "selUsable", selected->usable);
        ctx.ui.setString("inventory", "equipLabel",
                         ctx.texts.get(selected->equipped
                                           ? "ui.inventory.unequip"
                                           : "ui.inventory.equip"));
    }

    if (ctx.containerEntity.is_alive() &&
        ctx.containerEntity.has<gameplay::Inventory>()) {
        lootView.build(ctx.forms,
                       ctx.containerEntity.get<gameplay::Inventory>(),
                       nullptr);
        if (ctx.barterMode) {
            pushRows(lootView, "barter", ctx.vendorBuyMult);
            if (ctx.goldForm) {
                const auto& stock =
                    ctx.containerEntity.get<gameplay::Inventory>();
                ctx.ui.setString(
                    "barter", "vendorGold",
                    std::to_string(
                        gameplay::itemCount(stock, ctx.goldForm->id)));
                ctx.ui.setString(
                    "inventory", "goldText",
                    std::to_string(
                        gameplay::itemCount(*bag, ctx.goldForm->id)));
            }
        } else {
            pushRows(lootView, "container", 0.0f);
            ctx.ui.setString("container", "title",
                             ctx.texts.get("ui.container.loot"));
        }
    }
}

void GameHud::pushJournalModel(const HudContext& ctx) {
    if (!ctx.uiCreated) {
        return;
    }
    // Deterministic listing (§8): quests sorted by guid.
    vector<core::Guid> questIds;
    questIds.reserve(ctx.questLog.quests.size());
    for (const auto& [id, progress] : ctx.questLog.quests) {
        questIds.push_back(id);
    }
    std::sort(questIds.begin(), questIds.end());

    vector<::ui::UiRow> rows;
    for (const core::Guid& questId : questIds) {
        const quest::QuestProgress& progress =
            ctx.questLog.quests.at(questId);
        const auto* questForm = ctx.forms.find<quest::QuestForm>(questId);
        if (!questForm) {
            continue; // a mod removed the quest — skip, never fatal (§5)
        }
        ::ui::UiRow header;
        header.id = questId.toString();
        header.c0 = questForm->displayName;
        if (progress.status == quest::QuestStatus::Succeeded) {
            header.c1 = ctx.texts.get("ui.journal.done");
            header.tag = "done";
        } else if (progress.status == quest::QuestStatus::Failed) {
            header.c1 = ctx.texts.get("ui.journal.failed");
            header.tag = "done";
        }
        rows.push_back(std::move(header));
        if (progress.status != quest::QuestStatus::Active) {
            continue;
        }
        // Objectives = the tasks of the current state's branches.
        data::forEach<quest::QuestBranchForm>(
            ctx.forms, [&](const quest::QuestBranchForm& branch) {
                if (branch.state != progress.currentState) {
                    return;
                }
                data::forEach<quest::QuestTaskForm>(
                    ctx.forms, [&](const quest::QuestTaskForm& task) {
                        if (task.branch != branch.id) {
                            return;
                        }
                        ::ui::UiRow row;
                        row.id = task.id.toString();
                        row.c0 = task.displayName;
                        if (task.required > 1) {
                            row.c2 =
                                std::to_string(quest::taskProgress(
                                    ctx.questLog, questId, task.id)) +
                                " / " + std::to_string(task.required);
                        }
                        row.tag = "task";
                        rows.push_back(std::move(row));
                    });
            });
    }
    ctx.ui.setBool("journal", "empty", rows.empty());
    ctx.ui.setRows("journal", std::move(rows));
}

void GameHud::pushDialogueModel(const HudContext& ctx) {
    if (!ctx.dialogueRunner || !ctx.dialogueRunner->active()) {
        ctx.screenStack.close("dialogue");
        dialogueOptions_.clear();
        return;
    }
    const quest::DialogueNodeForm* line = ctx.dialogueRunner->currentLine();
    ctx.ui.setString("dialogue", "npcLine", line ? line->text : str {});
    dialogueOptions_ = ctx.dialogueRunner->options(ctx.evalContext);
    vector<::ui::UiRow> rows;
    u32 index = 1;
    for (const quest::DialogueNodeForm* option : dialogueOptions_) {
        ::ui::UiRow row;
        row.id = option->id.toString();
        row.c0 = std::to_string(index++) + ".  " + option->text;
        rows.push_back(std::move(row));
    }
    ::ui::UiRow leave;
    leave.id = "leave";
    leave.c0 = std::to_string(index) + ".  " +
               ctx.texts.get("ui.dialogue.leave");
    leave.tag = "leave";
    rows.push_back(std::move(leave));
    ctx.ui.setRows("dialogue", std::move(rows));
}

void GameHud::updateMenuClockLine(const HudContext& ctx) {
    const f64 hours = std::fmod(ctx.gameClock.gameHours(), 24.0);
    const i32 hh = static_cast<i32>(hours);
    const i32 mm = static_cast<i32>((hours - hh) * 60.0);
    const i32 day = static_cast<i32>(ctx.gameClock.gameDays()) + 1;
    char clock[16];
    std::snprintf(clock, sizeof(clock), "%02d:%02d", hh, mm);
    ctx.ui.setString("menu", "clockLine",
                     ctx.texts.format("ui.menu.day",
                                      { std::to_string(day), clock }));
}

void GameHud::reset() {
    invView = InventoryView {};
    lootView = InventoryView {};
    dialogueOptions_.clear();
}

} // namespace game
