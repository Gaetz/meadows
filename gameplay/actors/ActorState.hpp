#pragma once

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"
#include "gameplay/ability/GameplayTags.hpp" // FactionBounty.faction

// Small per-actor runtime state components (chantier 6). Reflected so the
// save layer captures them by field name through the captureActor sweep —
// NOT scene-level maps (a scene map dies on re-enter while the actor's
// inventory persists: a free-restock / bounty-amnesty exploit).

namespace gameplay {

// Vendor restock clock (D1): the game-time hour of the last inventory
// re-roll. The barter screen re-rolls the loadout when more than the
// restock interval has passed.
struct VendorState {
    f32 lastRestockHours { 0.0f };

    REFLECT_BEGIN(VendorState, void)
        REFLECT_FIELD(lastRestockHours)
    REFLECT_END()
};

// Crime bounty (D2) — the per-entity faction state CLAUDE.md §6.1
// sanctions as a thin component. Conditions can't see components, so the
// scene mirrors bounty > 0 into the Crime.Wanted tag (syncTag pattern).
//
// Per-faction (2026-07-13, the F-catalogue "bounty PAR FACTION" leftover):
// `bounty` stays the TOTAL (the legacy name-matched save field, the HUD
// and the fine gate); `perFaction` slices attribute it to the WITNESS's
// faction. sum(slices) <= total; any remainder is UNATTRIBUTED (an old
// save) and counts toward every faction — no amnesty on migration.
// Slices are not flat-reflectable: they persist as SavedBountyForm rows
// keyed by tag NAME (runtime tag ids aren't stable across runs).
struct FactionBounty {
    GameplayTag faction {};
    f32 amount { 0.0f };
};

struct Bounty {
    f32 bounty { 0.0f }; // total
    vector<FactionBounty> perFaction;

    REFLECT_BEGIN(Bounty, void)
        REFLECT_FIELD(bounty)
    REFLECT_END()
};

// An invalid faction (no witness tag) raises the total only —
// unattributed, so it angers every guard (the pre-migration behavior).
inline void addBounty(Bounty& bounty, GameplayTag faction, f32 amount) {
    bounty.bounty += amount;
    if (!faction.isValid()) {
        return;
    }
    for (FactionBounty& row : bounty.perFaction) {
        if (row.faction == faction) {
            row.amount += amount;
            return;
        }
    }
    bounty.perFaction.push_back({ faction, amount });
}

inline f32 unattributedBounty(const Bounty& bounty) {
    f32 attributed = 0.0f;
    for (const FactionBounty& row : bounty.perFaction) {
        attributed += row.amount;
    }
    const f32 rest = bounty.bounty - attributed;
    return rest > 0.0f ? rest : 0.0f;
}

// What `faction` holds against the actor: its own slice + the
// unattributed remainder.
inline f32 bountyToward(const Bounty& bounty, GameplayTag faction) {
    f32 amount = unattributedBounty(bounty);
    for (const FactionBounty& row : bounty.perFaction) {
        if (row.faction == faction) {
            amount += row.amount;
        }
    }
    return amount;
}

// Pays `faction` off: clears its slice AND the unattributed remainder
// (the fine settles everything nobody else specifically holds). Slices
// of OTHER factions survive — their guards stay hostile. An invalid
// faction clears the whole bounty (the unknown-arrester fallback).
inline void clearBountyToward(Bounty& bounty, GameplayTag faction) {
    f32 cleared = unattributedBounty(bounty);
    for (auto it = bounty.perFaction.begin();
         it != bounty.perFaction.end();) {
        if (!faction.isValid() || it->faction == faction) {
            cleared += it->amount;
            it = bounty.perFaction.erase(it);
        } else {
            ++it;
        }
    }
    bounty.bounty -= cleared;
    if (bounty.bounty < 0.0f) {
        bounty.bounty = 0.0f;
    }
}

// Follower runtime state (FOLLOWERS É0 — docs/CHANTIER-FOLLOWERS.md).
// Same pattern as VendorState/Bounty above: reflected, present on every
// actor, captured by the SavedStatsForm name-match sweep. Fields carry the
// `follower` prefix because SavedStatsForm field names must stay globally
// unique across every captured component (`level`/`active` are too
// generic to claim). Hour fields are GameClock game-time stamps (the
// VendorState.lastRestockHours idiom). §2.9 note: affinity is NOT a GAS
// attribute — it never moves through applyEffect.
struct FollowerState {
    bool followerActive { false };  // currently in the player's party
    f32 followerLevel { 1.0f };
    f32 followerAffinity { 0.0f };
    f32 followerHoursTogether { 0.0f };
    f32 followerContractExpiryHours { 0.0f };  // 0 = no contract (É10)
    f32 followerLastLevelSyncedFrom { 0.0f };  // player level at last sync
    f32 followerLastHomeUpgradeHours { 0.0f };
    f32 followerDownedRecoveryHours { 0.0f };  // convalescence timer (É3)
    // FOLLOWERS É9 (APPEND — ordinals stable): the group-command stance.
    // f32 like every sibling so the SavedStatsForm name-match sweep carries
    // it unchanged; read/written ONLY through gameplay::followerStance /
    // setFollowerStance (Followers.hpp — enum + one transition point).
    //   0 = follow (default), 1 = stay (hold position; the schedule takes
    //   over only on a DISMISS), 2 = attack (one-shot adoption of the
    //   player's current target at command time, then behaves as follow),
    //   3 = defend (never adopts on the player's initiative — rule 4 off).
    f32 followerStance { 0.0f };

    REFLECT_BEGIN(FollowerState, void)
        REFLECT_FIELD(followerActive)
        REFLECT_FIELD(followerLevel)
        REFLECT_FIELD(followerAffinity)
        REFLECT_FIELD(followerHoursTogether)
        REFLECT_FIELD(followerContractExpiryHours)
        REFLECT_FIELD(followerLastLevelSyncedFrom)
        REFLECT_FIELD(followerLastHomeUpgradeHours)
        REFLECT_FIELD(followerDownedRecoveryHours)
        REFLECT_FIELD(followerStance)
    REFLECT_END()
};

} // namespace gameplay
