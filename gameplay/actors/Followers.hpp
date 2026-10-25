#pragma once

// Umbrella over the follower rules — include the domain header you need
// instead: FollowerFollow (follow decision, aggro table), FollowerLife
// (downed, bleedout, convalescence), FollowerAffinity, FollowerProgression
// (classes, levels, perks, carry weight), FollowerParty (caps, stances,
// banter, comments, mercenaries). Pure and headless (§2.10), knobs in
// StatsTuningForm (§5); docs/CHANTIER-FOLLOWERS.md, docs/FOLLOWERS.md.

#include "gameplay/actors/FollowerAffinity.hpp"
#include "gameplay/actors/FollowerFollow.hpp"
#include "gameplay/actors/FollowerLife.hpp"
#include "gameplay/actors/FollowerParty.hpp"
#include "gameplay/actors/FollowerProgression.hpp"
