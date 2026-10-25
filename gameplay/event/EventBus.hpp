#pragma once

#include <functional>
#include <string_view>

#include "engine/core/Defines.hpp"
#include "engine/core/Hash.hpp"
#include "engine/ecs/World.hpp"
#include "gameplay/ability/GameplayTags.hpp"

// The gameplay event bus. A deterministic dispatcher that
// quests, scripts and abilities react to. It is Lua-agnostic — handlers are
// plain C++ callbacks; `meadows-script` registers Lua-forwarding handlers.
// Dispatch order = subscription order (§8 determinism).

namespace gameplay {

using EventKind = u32; // fnv1a of the event name, e.g. "OnDeath"
inline EventKind eventKind(std::string_view name) { return core::fnv1a(name); }

// The events the C++ side knows by name — the ones it dispatches (combat,
// death, anim) and the ones it subscribes to (dialogue options and trigger
// volumes fire them from data). Data matches events by HASH
// (AffinityRuleForm.event, CommentForm.event, QuestTaskForm.event…), so a
// misspelt name is a silently mute rule: `cooker validate` checks data
// against this registry. Data also mints events of its own
// (DialogueNodeForm.event, TriggerForm.event, wired to QuestTaskForm /
// QuestForm.startEvent), so an absent name is a warning, never an error.
// Dispatch and subscribe sites use these constants, never a literal.
namespace events {
// Dispatched from C++.
inline constexpr std::string_view AnimEvent = "AnimEvent";
inline constexpr std::string_view CallForHelp = "CallForHelp";
inline constexpr std::string_view OnAbilityUsed = "OnAbilityUsed";
inline constexpr std::string_view OnDeath = "OnDeath";
inline constexpr std::string_view OnDowned = "OnDowned";
inline constexpr std::string_view OnHitTaken = "OnHitTaken";
inline constexpr std::string_view OnParried = "OnParried";
inline constexpr std::string_view OnStagger = "OnStagger";
// Subscribed to by the scene; fired by dialogue options / trigger volumes.
inline constexpr std::string_view OnBuryFollower = "OnBuryFollower";
inline constexpr std::string_view OnDismissFollower = "OnDismissFollower";
inline constexpr std::string_view OnFollowerPreview = "OnFollowerPreview";
inline constexpr std::string_view OnFollowerStatus = "OnFollowerStatus";
inline constexpr std::string_view OnForgeUpgrade = "OnForgeUpgrade";
inline constexpr std::string_view OnForgeZone = "OnForgeZone";
inline constexpr std::string_view OnHireMercenary = "OnHireMercenary";
inline constexpr std::string_view OnLearnPerk = "OnLearnPerk";
inline constexpr std::string_view OnNoise = "OnNoise";
inline constexpr std::string_view OnPartyAttack = "OnPartyAttack";
inline constexpr std::string_view OnPartyDefend = "OnPartyDefend";
inline constexpr std::string_view OnPartyFollow = "OnPartyFollow";
inline constexpr std::string_view OnPartyStay = "OnPartyStay";
inline constexpr std::string_view OnPayFine = "OnPayFine";
inline constexpr std::string_view OnQuietZone = "OnQuietZone";
inline constexpr std::string_view OnRecruitFollower = "OnRecruitFollower";
inline constexpr std::string_view OpenBarter = "OpenBarter";
} // namespace events

// Every constant above, for the validator and the editor pickers. Adding
// an event = one constant + one row here.
inline constexpr std::string_view kEventNames[] = {
    events::AnimEvent,         events::CallForHelp,
    events::OnAbilityUsed,     events::OnDeath,
    events::OnDowned,          events::OnHitTaken,
    events::OnParried,         events::OnStagger,
    events::OnBuryFollower,    events::OnDismissFollower,
    events::OnFollowerPreview, events::OnFollowerStatus,
    events::OnForgeUpgrade,    events::OnForgeZone,
    events::OnHireMercenary,   events::OnLearnPerk,
    events::OnNoise,           events::OnPartyAttack,
    events::OnPartyDefend,     events::OnPartyFollow,
    events::OnPartyStay,       events::OnPayFine,
    events::OnQuietZone,       events::OnRecruitFollower,
    events::OpenBarter,
};

inline bool isKnownEvent(std::string_view name) {
    for (const std::string_view known : kEventNames) {
        if (known == name) {
            return true;
        }
    }
    return false;
}

// A generic gameplay event. The fixed payload (source/target/tag/value/name)
// covers the common cases (hit, death, activate, trigger, task progress…)
// without a typed struct per event, and maps cleanly to a Lua table.
struct Event {
    EventKind kind { 0 };
    ecs::Entity source {};      // who caused it (attacker, activator)
    ecs::Entity target {};      // who it happened to (victim, activated)
    GameplayTag tag {};         // optional categorizing tag
    f32 value { 0.0f };         // optional scalar (damage, progress…)
    str name {};                // optional string arg (e.g. a data-task arg)
};

using EventHandler = std::function<void(const Event&)>;
using SubscriptionId = u32;

class EventBus {
public:
    SubscriptionId subscribe(EventKind kind, EventHandler handler);
    // Receives EVERY event, whatever its kind — what the quest
    // system needs (task events and quest startEvents are open, data-
    // defined vocabulary; per-name C++ subscriptions can't know them).
    SubscriptionId subscribeAll(EventHandler handler);
    void unsubscribe(SubscriptionId id);

    // Calls every handler for `event.kind` (plus the subscribeAll ones),
    // in subscription order. Re-entrant: a handler ADDED during dispatch
    // first fires on the next one; a handler REMOVED during dispatch is
    // not called after its removal.
    void dispatch(const Event& event) const;

private:
    struct Sub {
        SubscriptionId id;
        EventKind kind;
        EventHandler handler;
    };
    vector<Sub> subs;
    SubscriptionId nextId { 1 };
};

} // namespace gameplay
