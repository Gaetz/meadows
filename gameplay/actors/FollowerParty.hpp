#pragma once

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"              // bond / banter pair guids
#include "gameplay/ability/GameplayTags.hpp" // GameplayTag (comment matching)

namespace gameplay {

struct StatsTuningForm;
struct FollowerState;

// ---- Party caps, stances, banter, ambient comments ----------------------
// Reused systems (§2.11): the caps read ActorForm.followerCategory
// (data) against StatsTuningForm knobs (§5); the stance is one more
// FollowerState field riding the SavedStatsForm name-match sweep; banter
// and comments are child/top-level data records (the AffinityRuleForm /
// QuestTaskForm matching) on GameClock hour stamps (the VendorState
// idiom). All pure and doctested; the game side only resolves entities.

struct FollowerBondForm;
struct BanterForm;
struct CommentForm;

// The party census by follower category. "mount" is EXEMPT from both caps
// (docs/FOLLOWERS.md §8 — it never counts); "minor" fills the minor cap;
// anything else (the authored "major", or a modded typo) counts as major —
// the strict bucket, so bad data can never bypass the cap.
struct PartyCounts {
    i32 majors { 0 };
    i32 minors { 0 };
};
void countPartyMember(PartyCounts& counts, const str& category);

// The recruit gate (checked at recruit time): does one more
// `category` fit under the caps? Caps come in as integers (the f32 tuning
// knobs truncated by the caller).
enum class RecruitVerdict : u8 { Ok, MajorsFull, MinorsFull };
RecruitVerdict canJoinParty(const str& category, const PartyCounts& counts,
                            i32 majorCap, i32 minorCap);

// The group-command stance (docs/FOLLOWERS.md §7, "strategy adjustable
// through dialogue"). Persisted as FollowerState.followerStance (f32 — the
// save sweep); read/written ONLY through these two (the enum-over-bools
// rule: one vocabulary, one transition point). Out-of-range floats decay
// to Follow — a modded save never wedges a follower.
enum class FollowerStance : u8 { Follow = 0, Stay = 1, Attack = 2, Defend = 3 };
FollowerStance followerStance(const FollowerState& state);
void setFollowerStance(FollowerState& state, FollowerStance stance);

// Anti-repeat clock, one per BanterForm/CommentForm guid in a
// RUNTIME-ONLY map (v1, stated: resets on load — acceptable; the oneShot
// flag is not persisted either). Hours are GameClock game-time stamps.
struct CommentClock {
    bool fired { false };
    f64 lastHours { 0.0 };
};

// Does `comment` match this bus event? Name equality, optional filterTag
// descent (the QuestTaskForm matching — an unknown filter or a tagless
// event fails), the minValue floor (OnQuietZone enter=1 / leave=0), and
// the optional sourcePlayer party filter (the AffinityRuleForm idiom).
bool commentMatches(const CommentForm& comment, u32 eventKind,
                    const GameplayTag& eventTag, f32 eventValue,
                    bool sourceIsPlayer, const GameplayTagRegistry& tags);

// The anti-repeat + chaining gate (docs/FOLLOWERS.md §6.1): never in
// sneak; a oneShot never refires; a fired comment waits cooldownHours;
// with requiresComment set, the PREREQUISITE must have fired at least
// minGapHours ago (the ordered-chaining contract).
bool decideComment(const CommentForm& comment, f64 nowHours, bool sneaking,
                   const CommentClock& own, const CommentClock& prerequisite);

// The pair-affinity lookup over the authored bonds (symmetric — {a,b} in
// either order); no bond = 0. V1: bonds are initial values only (no
// runtime mutation — stated).
f32 pairAffinity(const vector<const FollowerBondForm*>& bonds,
                 const core::Guid& a, const core::Guid& b);

// Is this banter's PAIR the two given actors (either order)?
bool banterPairMatches(const BanterForm& banter, const core::Guid& x,
                       const core::Guid& y);

// The banter gate: pair affinity >= minPairAffinity, oneShot respected,
// cooldownHours elapsed. The caller checks presence/distance/combat/sneak
// and the global banterIntervalHours cadence.
bool decideBanter(const BanterForm& banter, f32 pairAffinityValue,
                  f64 nowHours, const CommentClock& clock);

// ---- Mercenaries --------------------------------------------------------
// Reused systems (§2.11): the recruit/dismiss contract does the joining
// and the going-home; the gold moves the payFine way (removeItem, handler-
// side so a refusal stays free); the contract clock is one GameClock
// game-hour stamp on FollowerState (the VendorState.lastRestockHours
// idiom, followerContractExpiryHours);
// the hire/renew options ride the dialogue-event channel (OnHireMercenary);
// the toasts ride the loc pipeline. This block is the PURE math only,
// doctested headless (§2.10).

// The hire price, v1 formula (documented — reputation/factions join when
// that system exists, the Bounty score-mirror being the stated template):
//   price = base × (1 + (level - 1) × mercenaryLevelPricePerLevel)
//                × (1 + min(gold / mercenaryWealthPivot, 1)
//                       × mercenaryWealthFactor)
// rounded to the nearest gold piece. Level clamps to >= 1 (a fresh save's
// level 1 pays exactly `base` when broke); gold clamps to >= 0; the wealth
// axis SATURATES at the pivot (a dragon's hoard never scales past
// base × levelTerm × (1 + factor)); a non-positive pivot disables the
// wealth axis (modded-data safety).
i32 mercenaryPrice(f32 basePrice, f32 playerLevel, i32 playerGold,
                   const StatsTuningForm& tuning);

// Where a contract stands at `nowHours` (game hours, the GameClock):
//   None    — no stamp (expiryHours <= 0): not under contract.
//   Engaged — running, expiry further than the warning window.
//   Warning — inside the last `warningHours` before expiry: the sweep
//             toasts ONCE (the once-flag is controller runtime, v1 —
//             stated: not persisted, a reload may re-warn).
//   Expired — nowHours >= expiryHours: auto-dismiss home, stamp cleared.
enum class ContractPhase : u8 { None, Engaged, Warning, Expired };
ContractPhase contractPhase(f64 nowHours, f32 expiryHours, f32 warningHours);

// The new expiry stamp for a hire OR a renewal: `contractDays` game days
// on top of max(now, current expiry) — renewing early ADDS to the running
// contract (no paid hours lost), renewing late (or hiring fresh, expiry
// 0) starts from now.
f32 extendContract(f64 nowHours, f32 expiryHours, f32 contractDays);

} // namespace gameplay
