# Chantier 3 — Vivant : « explorer, combattre, parler »

> **FAIT (2026-07-06) — exécuté d'une traite sur directive dev** (« passe
> à la suite, je testerai plus tard » — même mode que le chantier 2).
> 246 tests / 77 845 assertions verts. Validation visuelle dev EN ATTENTE
> (liste « quoi tester » dans le rapport de fin de session). Écarts
> d'exécution et pièges payés en fin de fichier. État global :
> `docs/MEADOWS-PLAN.md`.

## Contexte

Troisième chantier : le monde devient VIVANT. Les PNJ exécutent leurs
emplois du temps (dormir/manger/travailler via le mobilier), perçoivent,
se battent ; le joueur interagit (Prendre/Parler/Dormir), se bat en 3D,
entend le monde (cues → FX + audio). C'est la boucle Skyrim minimale, et
le chantier qui absorbe le reliquat de la Phase 8 (IA ennemie, aubergiste,
marchand) + l'étude NarrativePro §C.1 (grants/loadout/tags/dialogue sur
ActorForm).

## Acquis (ne pas recréer)

- **Anim** : graphe data-driven + events timeline (Footstep déjà tirés,
  SANS consommateur) ; clips UAL riches (Sword_Attack, Hit_Chest, Death01,
  Sitting_*, Interact, Punch_*, Spell_*) — le combat et le mobilier ont
  déjà leurs animations.
- **GAS complet** (Phases 3-7) : abilities avec coûts/cooldowns/tags,
  typed damage → armor → health+posture, staggers, statuts, blessures.
  La CombatArena 2D est la référence de câblage (dodge = ability, hit =
  effect) — PORTER, pas réinventer.
- **Cues** : CueRegistry + CueTable (fallback hiérarchique) + CueForm +
  `fx::ParticleSim` (preuve étincelles 2D). Émission déjà posée dans
  applyEffect côté CombatArena.
- **Audio** : seam miniaudio (bus, play 2D/3D, crossfade) ; SoundForm +
  variantes enfants (H1) — le résolveur manque.
- **Schedules** : ScheduleForm + evaluateSchedule (doctesté, minuit,
  overrides) ; AiPackageForm ; FurnitureForm + FurnitureOccupancy
  (claim/release) ; vue debug drawSchedules dans l'EditorScene ; GameClock
  (Phase 6) PAS ENCORE branché sur LandscapeScene (game time = real time).
- **Nav** : interface `nav::Navigator` + GridNavigator (A* 2D). Recast =
  la vraie brique de ce chantier.
- **PNJ 3D** : pipeline complet (spawn Forms → skin/graphe/patrouille,
  chantier 1 B6) — la patrouille est l'embryon à remplacer par les
  packages.
- **Physique** : raycasts + shape casts à ajouter (façade), colliders
  statics/terrain déjà là.

## Les briques proposées

### B1 — Horloge de jeu + interaction de base
- Brancher GameClock sur LandscapeScene (timescale, l'heure pilote DÉJÀ le
  ciel : lier sky.timeOfDay au clock) ; `tickCharacter` passe au vrai
  gameDt.
- Raycast d'interaction générique (E) : Prendre (ItemMarker → inventaire),
  Parler (ActorMarker → placeholder dialogue), Utiliser (FurnitureMarker).
  Le prompt B7 se généralise (portes = un cas parmi d'autres).

### B2 — Navmesh Recast/Detour
- CPM recast (pin) derrière `nav::Navigator` (l'interface EXISTE — impl
  `RecastNavigator` dans world/ai/). Génération par cellule au chargement
  (sync v1, worker si ça pique), depuis les mêmes triangles que les
  colliders (MeshCache CPU + heightfield terrain échantillonné).
- `findPath` async-ready (pattern worker/queue si besoin). Doctests :
  chemin autour d'un obstacle, chemin sur terrain patché.

### B3 — Packages IA + schedules EXÉCUTÉS
- `ScheduleAgent` (composant runtime) : évalue au changement d'heure,
  empile les intents (interruption combat/dialogue, reprise) — la note
  HORIZONTAL-PASS § schedules/mobilier/nav est le contrat.
- Packages v1 : wander (points aléatoires navmesh), travel (marker),
  useFurniture (nav → claim → anims Sitting_Enter/Loop/Exit → effet GAS →
  release), sleep/eat par-dessus useFurniture.
- Le Villager passe de PatrolWalker aux schedules (ScheduleForm dans
  village.toml : dort la nuit dans la maison, flâne le jour) — **LA
  preuve : sa journée se déroule toute seule**.

### B4 — Cues + audio branchés
- Résolveur SoundForm (variantes par poids, jitter, chemin AssetDatabase)
  → `audio::play` 3D ; handlers standard des cues : particule (ParticleSim
  → quads caméra 3D — extension du rendu, quads dans la passe opaque ou
  alpha) + son + shake.
- Points d'émission : applyEffect (dégâts/soins), Footstep (events anim →
  cue par matériau de sol : splat weights CPU déjà disponibles), portes.
- Ambiance : un bed extérieur (vent/oiseaux par météo — la météo pilote
  déjà le visuel) + un bed intérieur ; crossfade au travel B7.

### B5 — Perception + IA de combat 3D
- Perception : cône de vue + raycast d'occlusion (physique), ouïe
  (événements de cues), mémoire de dernière position. Composant
  `Perception` + doctests headless.
- IA combat : port du chase/attack 2D (CombatArena) sur navmesh —
  distances d'engagement, strafe, abilities GAS (coûts/cooldowns), fuite
  sous seuil de santé, appel à l'aide (factions/tags).

### B6 — Combat 3D joueur
- Attaque mêlée : ability GAS + fenêtre de dégâts par event d'anim (« Hit »
  → shape cast de l'arme via la façade — à ajouter : `shapeCast`) ;
  blocage directionnel v1 simple ; le mannequin UAL a Sword_Attack/Hit_*/
  Death01. HUD minimal : santé/énergie/posture en overlay (ImGui — RmlUi
  au chantier « interfaces »).
- Ennemi de démo (bandit ActorForm + loadout §C.1 minimal : grants
  d'abilities par records enfants `ActorGrantForm` — PREMIÈRE brique de la
  fiche de personnage NarrativePro).
- Mort : Death01 + despawn différé ; le PNJ villageois fuit le combat.

### B7 — PNJ marchand + aubergiste (reliquat Phase 8)
- Barter : port du système 2D (les données existent) — UI ImGui v1.
- Aubergiste : dormir dans le lit (FurnitureForm) = rest Phase 7 (récup
  santé/énergie/essence + avance l'horloge).

### B8 — Clôture
- CHANTIER-3 annoté, MEADOWS-PLAN (coches E/F/H/I), HORIZONTAL-PASS,
  userdoc (schedules-and-furniture surtout), mémoire agent.

## Réalisé — journal d'exécution (2026-07-06)

| Brique | État | Notes |
|---|---|---|
| B1 horloge + interaction | ✅ | GameClock (timescale 12) possède sky.timeOfDay ; E générique : porte / prendre (inventaire) / parler (placeholder) / mobilier |
| B2 navmesh | ✅ **écart** | Recast REPORTÉ (garde-fou appliqué) → `world/ai/TerrainNavigator` : A* grille 1 m projeté terrain, obstacles = AABB colliders statics +0.4 m, doctesté |
| B3 schedules exécutés | ✅ | Ré-évaluation par slot de 10 min-jeu ; wander/travel/useFurniture (claim + tag Sitting)/guard ; la journée du Villager se déroule seule (village.toml) |
| B4 cues + audio | ⛔ REPORTÉ | AUCUN asset audio dans le dépôt — même workflow que le kit : le dev dépose les sons, la brique se fait à ce moment-là (le seam miniaudio + SoundForm attendent) |
| B5 perception + IA combat | ✅ simplifié | Chase à vue (distance + raycast LOS physique) sans composant Perception séparé ; repath 1 s, attaque à 1.8 m / 1.6 s via GAS |
| B6 combat joueur | ✅ simplifié | LMB mêlée : cooldown 0.7 s, cible la plus proche ≤ 2.4 m dans le cône, `weaponDamageEvent` → `applyDamage` (§2.9) ; mort = Death01 (tag State.Dead, gate anim) + gel ; PAS d'ability GAS formelle ni de fenêtre par event d'anim (v1 sans corps visible) — À FORMALISER quand le combat s'épaissit |
| B7 marchand + repos | ✅ partiel | Repos : E sur mobilier → `gameplay::sleep()` Phase 7 (lit 8 h / siège 1 h, fondu au noir, l'horloge saute, le ciel suit) ; **barter REPORTÉ** (UI = chantier « interfaces ») |
| B8 clôture | ✅ | Ce fichier, MEADOWS-PLAN, mémoire |

**Contenu data** : `village.toml` — RustySword/BanditClub (WeaponForm),
Bandit (ActorForm + `ActorTagForm` Faction.Bandits → hostile, réf cellule
(1,5)), SimpleBed (FurnitureForm category=bed, visuel caisse aplatie —
remplacer quand un vrai lit arrive) ; `adventure.toml` — état anim Death01
(depuis idle/walk, requiredTag State.Dead), Sitting_Idle_Loop.

**Pièges payés** :
- `ActorTagForm` = le pattern §C.1 (records enfants par `parent`) — les
  grants/loadout suivront le même moule.
- Les gates d'anim par tag (`GraphInstance::setTagCheck`) suffisent pour
  sitting ET death — aucun nouveau runtime d'anim.
- `tickCharacter` par PNJ : gameDt = dt × timescale, le MÊME pipeline que
  le joueur (régén, survie) — pas de tick « light » parallèle.
- Le zombie `cl.exe` (PDB lock C1041) : si un build échoue en boucle sur
  vc143.pdb, chercher un cl.exe orphelin figé (CPU constant) et le tuer.
- `RefId.base` est un FormHandle : lookup typé = `typeOf(handle)->isA(...)`
  + `static_cast`, pas `find<T>` (qui prend un Guid).

## Garde-fous

- Le GAS est COMPLET : toute mécanique de combat = abilities/effects
  existants, jamais de dégâts « à la main » (§2.9).
- Cues : le sim ÉMET, le frontend résout (headless no-op) — ne pas
  brancher l'audio/FX directement dans gameplay/.
- Recast : si l'intégration dépasse ~une session, GridNavigator projeté
  sur le terrain 3D est un fallback acceptable pour B3 (noter et avancer).
- Les événements d'anim sont LE pont anim→gameplay (fenêtres de hit) —
  pas de timers parallèles.
- NPC nombreux viendront : garder buildNpc générique (déjà le cas) mais
  ne PAS optimiser (instancing skinned) avant que ça pique.
