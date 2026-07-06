# Chantier 5 — Persistance : « sauvegarder = une couche de patches »

> **FAIT (2026-07-06) — exécuté d'une traite sur approbation du plan**
> (même mode que les chantiers 2-4). 268 tests / 78 050 assertions verts ;
> boucle F5→F9 vérifiée EN JEU (12 records, rechargement complet).
> Validation visuelle dev EN ATTENTE. Journal d'exécution et pièges payés
> en fin de fichier. État global : `docs/MEADOWS-PLAN.md`.

## Contexte

Cinquième chantier (`docs/MEADOWS-PLAN.md` §K, l'ex-Phase 10) : la save et
le streaming sont le MÊME chantier. L'invariant non négociable (§2.4/§5) :
**une sauvegarde est un plugin ordinaire**, résolu APRÈS toute la pile —
mêmes records, même résolveur, même TomlWriter, aucun système parallèle.
Tout l'outillage existe (prouvé par exploration + agent Plan) : resolver
field-level, `Record{formId, typeId, creates, fields}`, TomlWriter (f64 ok,
`new = true` ok), `Guid::combine` (conçu pour ça), convention records
enfants `parent` (§C.1), et le contrat §6 : **persister les BaseValues +
les effets duratifs actifs, recalculer les currents au load**.

Ce que le chantier livre : sauvegarder/charger la partie (position, stats,
inventaire/équipement, effets actifs, blessures, références
ramassées/déplacées, PNJ morts avec leur loot, horloge/worldspace), la
mémoire des cellules déchargées (looter une caisse, s'éloigner, revenir —
sans même sauver sur disque), et un streaming lissé.

**Décision de cadrage — « streaming async » v1 = spawn budgété.** Les
données de cellules sont déjà en RAM (FormDatabase) : il n'y a AUCUN IO
disque à déporter sur un worker, et le spawn flecs doit rester sur le main
(règles Phase 5). Le vrai async (fichiers cuits par cellule + worker)
viendra avec le volume ; ici on lisse : 1 cellule spawnée/frame au
franchissement de frontière (l'anneau initial et les travels restent
synchrones derrière le fondu).

## Corrections issues de la validation (à respecter)

- **`ActiveEffect` ne stocke PAS le guid de son EffectForm** et une form
  `attribute2` crée DEUX rows : `SavedEffectForm` reflète donc
  `ActiveEffect` DIRECTEMENT (attribute, op, magnitude, infinite,
  remaining, period, sinceLastTick, grantedTag, decayOnExpiry/PerHour,
  expiryMagnitude, gameTime) et se restaure par un nouveau
  `restoreActiveEffect(system, row, tags)` (push + refcount du tag +
  `nextEffectId` + un `recomputeCurrent` final) — PAS par `applyEffect`.
- **`CombatState` n'est pas réfléchi** (4 f32) : ajouter son REFLECT.
  Exclure `AttributeSet.damage` (méta-attribut transient) de la capture.
- **`initializeActorStats` remet les vitals à fond ET efface State.Dead**
  (CharacterTick.cpp:162-169) : `applySavedState` court strictement après,
  puis appelle `gameplay::updateLifeState` (Combat.cpp:5) pour re-dériver
  Dead d'une santé à 0 (le tag n'est jamais stocké).
- **Le sentinel « cet acteur a été capturé » = l'existence de son
  `SavedStatsForm`** (pas des SavedItemForms : un inventaire vide sauvé
  re-roulerait son loadout sinon).
- **Capture à la main (`Record{...}` construits directement), PAS via
  EditSession** : `createForm` mint un guid aléatoire (incompatible avec
  l'identité déterministe) et `setField` refuse les guids inconnus de la
  base. Reproduire la sémantique diff d'`exportPlugin` (comparaison
  `reflect::Value`, skip des champs `Transient`) dans un petit helper.
- **Guids déterministes par `Guid::combine`** : stats =
  `combine(kSavedStatsNs, refGuid)` ; item =
  `combine(combine(kSavedItemNs, refGuid), itemGuid)` ; rows ordinales
  (effets/blessures) combinent un index. Re-save idempotente, TOML
  diffables.
- **Piège des enfants de prefab** : leurs ReferenceForms dérivées
  (`combine(instance, template)`, Spawner.cpp:154) n'existent dans AUCUN
  plugin → un PATCH de save serait un orphelin QUE LE RESOLVER JETTE.
  Fix : la save émet des records `creates=true` COMPLETS pour les enfants
  touchés + l'expansion de prefab saute un enfant dont le guid dérivé
  existe déjà dans la base (`handleOf(...).isValid()`).
- Le resolve d'une save inonde `ResolveReport.conflicts` (c'est voulu :
  base-vs-override) — ne jamais gater dessus.
- Au load, `WorldStateForm` doit OVERRIDE le reset codé en dur de
  l'horloge dans onEnter (LandscapeScene.cpp:296 : 10 h, timescale 12).

## Followers — anticipé ici, implémenté plus tard (décision dev)

Les followers (essentiels au jeu) n'exigent AUCUN nouveau type d'acteur :
- Les références **sans cellule (cell = 0) sont persistantes** (spawnées
  à l'entrée, ignorées du streamer — le statut du joueur aujourd'hui), et
  ce chemin est générique (passe persistante d'onEnter + refreshNpcs).
- **Recruter = patcher `ReferenceForm.cell` → 0** (+ retirer la relation
  `InCell` de l'entité pour survivre à l'unload de sa cellule d'origine) ;
  **congédier / emménager = patcher `cell` → la cellule cible** (+
  position). Deux patches de champ, §5 pur — la save les porte gratuitement.
Ce que CE chantier garantit pour ça :
1. `captureReference` diffe AUSSI le champ `cell` (y compris → guid nul
   et → autre cellule) — vérifier que TomlWriter/resolver véhiculent un
   guid nul en patch.
2. La passe persistante et `finalizeActorSpawn` restent génériques pour
   tout acteur sans cellule (pas de cas « Player » en dur).
3. Doctest (dans SaveRoundTripTest ou CellDeltaTest) : une référence
   re-domiciliée par la couche de save spawn dans sa NOUVELLE cellule ;
   une référence passée à cell=0 spawn en persistant à sa position sauvée.
Le futur chantier followers n'ajoutera que le package IA `follow` + un
tag §C.1 — zéro travail de persistance restant.

## Les briques

### B1 — Types de Forms + fixes réflexion
- `gameplay/save/SaveForms.{hpp,cpp}` (lib meadows-gameplay) :
  `SavedStatsForm { parent + ~35 champs plats : 9 CoreAttributes, bases
  AttributeSet (sans `damage`), Resonance ×3, Survival ×3, StatusBuildup
  ×9, CombatState ×4, Equipment ×5 guids }`, `SavedEffectForm` (miroir
  ActiveEffect), `SavedItemForm { parent, item, count }`,
  `SavedInjuryForm { parent, type, part, severity,
  recoveryHoursRemaining }`, `WorldStateForm { gameSeconds f64, timescale,
  activeWorldspace guid, playerYaw, playerPitch, playMode,
  weatherSelected }` + registration (+ AllForms + cooker Main).
- REFLECT sur `CombatState` ; `restoreActiveEffect` dans gameplay/ability.
- Doctest `SaveFormsTest` : résolution §5 des nouveaux types
  (create/patch/childrenOf).

### B2 — SaveCapture (headless, gameplay/save)
- `captureActor(entity, refGuid) -> vector<Record>` : lecture des
  composants par réflexion (matching par nom de champ vers
  SavedStatsForm), effets actifs → SavedEffectForms, Inventory →
  SavedItemForms, Injuries → SavedInjuryForms. Ordre déterministe (§8 :
  tri par guid).
- Diff de ReferenceForm : `captureReference(entity, forms) -> Record?`
  (patch portant SEULEMENT les champs qui diffèrent : position, enabled,
  count, **cell** — la sémantique d'exportPlugin, champs Transient
  exclus). Le diff de `cell` (→ nul ou → autre cellule) est le socle des
  futurs followers/déménagements.
- Doctest `SaveCaptureTest` (pattern EditSessionTest).

### B3 — SaveApply + le seam de spawn — LA preuve headless
- `applySavedState(entity, refGuid, db, tags, ...)` : set des champs par
  réflexion, restoreActiveEffect ×N, refill Inventory/Equipment,
  `updateLifeState` final.
- Scène : extraire `finalizeActorSpawn(entity, refGuid)` =
  `initializeActorStats` → SavedStatsForm existe ? `applySavedState` :
  `applyLoadout` — appelé aux DEUX sites (bloc kit joueur ~L326-353 et
  `refreshNpcs` via le pattern pendingLoadouts — jamais de set<> dans le
  `.each()`, piège LOCKED_STORAGE).
- Doctests `SaveApplyTest` + **`SaveRoundTripTest`** : monde A (dégâts,
  effet duratif à moitié écoulé real-time + un game-time, items,
  blessure) → capture → TOML → parse → resolve(base+save) → monde B →
  comparaison champ à champ par réflexion ; PNJ à santé 0 re-dérive
  State.Dead ; loadout non re-roulé.

### B4 — Mémoire des cellules (couche pending en RAM)
- Hook `std::function<void(data::FormHandle cell, ecs::Entity cellEntity)>
  beforeUnload` sur `CellLoader` (invoqué en tête d'unloadCell — couvre
  l'éviction du streamer, unloadAll et les travels ; world/ ne gagne que
  <functional>).
- La scène installe le hook (elle construit le loader, réinstallé à
  chaque onEnter) : capture des entités de la cellule
  (`query .with<ecs::InCell>(cellEntity)`) → **couche pending en
  mémoire** (map refGuid → records, remplacée par cellule).
- Lookup à l'apply : pending d'abord, FormDatabase ensuite (défini dès
  B3). Pickup d'item : patch `enabled=false` poussé dans la pending
  immédiatement (remplace le simple destruct).
- Doctest `CellDeltaTest` : load → loot/move → unload → reload → l'état
  revient, sans disque.

### B5 — Fichiers de save + load
- Save : flush pending + capture des cellules chargées + du joueur +
  `WorldStateForm` → `Plugin` → `writePluginToml` →
  `saves/<slot>.toml` (à côté de l'exe, même écart assumé que mods/).
  Binaire cuit = option future (le cooker sait déjà).
- Load : `pendingSavePath` membre → re-enter de la scène ; onEnter
  append le plugin de save APRÈS la pile avant `resolve` ; WorldStateForm
  restaure horloge/worldspace/caméra/mode ; position joueur via le patch
  de sa ReferenceForm persistante.
- Console `save [nom]` / `load [nom]` + **F5/F9** (touches libres).
- Doctest : liste/écriture/lecture de slots (round-trip fichier).

### B6 — UI
- Menu pause : bouton **Save** ; menu principal : **Load** dé-grisé →
  écran `saves.rml` (le modèle rows existant : nom + date, clic = load).
  `UiScreenForm` + records ui.toml.
- Preuve : boucle complète jouer → F5 → quitter → relancer → Load →
  reprendre exactement là.

### B7 — Persistance des enfants de prefab
- Save : `creates=true` complets pour les enfants dérivés touchés ;
  `Spawner::spawn` (expansion H8) saute un template dont le guid dérivé
  existe déjà dans la base. Doctest `PrefabChildSaveTest`.
- (Tirable avant B5 si le loot posé via prefabs compte pour la démo.)

### B8 — Streaming lissé (spawn budgété)
- File de cellules en attente dans la scène (ou CellStreamer) : au
  changement d'anneau, 1 `loadCell`/frame ; l'anneau initial et les
  travels restent synchrones (derrière le fondu). Compteur F1.
- Doctest : la file se draine dans l'ordre déterministe.

### B9 — Clôture
- `docs/CHANTIER-5.md` (journal + pièges), MEADOWS-PLAN (K ✅, chantier
  suivant), userdoc (nouvelle page `saving.md` : format de save = plugin,
  slots, ce qui est capturé ; lien depuis data-model), mémoire agent.

## Fichiers principaux

- Nouveaux : `gameplay/save/SaveForms.{hpp,cpp}`,
  `gameplay/save/SaveState.{hpp,cpp}` (capture/apply/restoreActiveEffect
  côté gameplay, headless), `game/SaveGame.{hpp,cpp}` (fichiers/slots,
  meadows-runtime), `game/data/base/ui/saves.rml`, tests
  `SaveFormsTest/SaveCaptureTest/SaveApplyTest/SaveRoundTripTest/
  CellDeltaTest/PrefabChildSaveTest`.
- Étendus : `gameplay/stats/Damage.hpp` (REFLECT CombatState),
  `gameplay/ability/AbilitySystem.*` (restoreActiveEffect),
  `world/streaming/CellLoader.{hpp,cpp}` (beforeUnload),
  `world/scene/Spawner.cpp` (skip enfant dérivé existant),
  `game/scenes/LandscapeScene.{hpp,cpp}` (finalizeActorSpawn,
  pendingSavePath, pending layer, F5/F9, console, WorldStateForm au
  resolve), `game/AllForms.cpp`, `tools/cooker/Main.cpp`, ui.toml +
  pause/main-menu.rml.

## Vérification

Par brique : build + suite headless complète (258+) + smoke-run. La
preuve du chantier = `SaveRoundTripTest` headless (B3) puis la boucle
visuelle B6 (jouer → tuer le bandit → looter → F5 → relancer → Load :
le bandit est mort, son gourdin est dans ton sac, l'heure est la bonne).

## Réalisé — journal d'exécution (2026-07-06)

| Brique | État | Notes |
|---|---|---|
| B1 Forms + réflexion | ✅ | 5 types (`gameplay/save/SaveForms`) ; REFLECT sur CombatState ; `restoreActiveEffect` implémenté avec les APIs PUBLIQUES du GAS (aucune modif d'AbilitySystem nécessaire) ; guids déterministes `Guid::combine` (re-save idempotente, TOML diffables) |
| B2 capture | ✅ | `captureActor` (pont réflexion par NOM de champ — `copyMatchingFields` marche dans les deux sens), items triés par guid (§8) ; `game::captureReference` diffe position/rotation (acteurs), cell (toujours — contrat followers), enabled |
| B3 apply + seam | ✅ | `applySavedState` (champs par réflexion, effets, conteneurs, `initializeCurrent`+`recomputeCurrent`+`updateLifeState` — un mort charge mort) ; `finalizeActorSpawn` = LE seam unique joueur+PNJ (pending d'abord, base résolue ensuite, loadout sinon) ; round-trip headless champ à champ prouvé |
| B4 couche pending | ✅ | `CellLoader.beforeUnload` + `spawnFilter` ; `PendingSaveLayer` (records par référence, matérialisation typée à la demande) ; pickup = disable immédiat ; caisse lootée qui reste lootée SANS disque (CellDeltaTest) |
| B5 fichiers | ✅ | `saves/<slot>.toml` (writePluginToml — une save EST un fichier plugin) ; load = re-enter de scène avec la save résolue en DERNIÈRE couche ; WorldStateForm override l'horloge/worldspace ; capsule placée en absolu (pattern travel — les intérieurs marchent) ; console `save`/`load`, **F5/F9** |
| B6 UI | ✅ | Pause : Save (slot horodaté) + Load ; menu principal : Load dé-grisé → `saves.rml` (nom + date, clic = charger) |
| B7 enfants de prefab | ✅ | Le piège orphelin FERMÉ : capture/disable MATÉRIALISENT l'enfant dérivé en record `creates` complet ; l'expansion s'efface si le record existe en base, et consulte le veto pending en session (`SpawnContext.filter`) |
| B8 streaming lissé | ✅ | Budget de chargement sur `CellStreamer::update` (1 cellule/frame au franchissement ; anneau initial + travels entiers derrière le fondu) ; l'anneau incomplet reprend au prochain appel |
| B9 clôture | ✅ | Ce fichier, MEADOWS-PLAN, userdoc `saving.md`, mémoire |

**Pièges payés** :
- `ActiveEffect` ne connaît PAS sa form source → la save reflète la ROW
  (l'id d'attribut fnv1a est stable, il se persiste tel quel — aucun
  reverse-lookup nom possible pour les stats dérivées).
- `initializeActorStats` remet tout à neuf ET efface State.Dead —
  l'apply court après et re-dérive (`updateLifeState`).
- Patch sur un guid dérivé de prefab = orphelin JETÉ par le resolver —
  toujours matérialiser en `creates` complet.
- Le spawn budgété doit laisser l'anneau « incomplet » (ringValid=false)
  sinon l'early-out du streamer gèle le remplissage.
- Capsule d'un load : position ABSOLUE (pattern travel), jamais
  enterPlayMode (qui re-ancre les pieds sur le terrain — faux en
  intérieur).

## Garde-fous

- **JAMAIS de mécanisme parallèle** : la save est un plugin ; « forcer
  une valeur » = une couche de plus (§2.4/§5.1). Pas de /save dans
  data/plugins.
- §6 : bases + effets actifs seulement ; les currents se RECALCULENT
  (recomputeCurrent au bout d'applySavedState/restore).
- Déterminisme §8 : capture triée par guid ; les rolls de loadout ne
  re-roulent pas sur un acteur capturé.
- Ne PAS anticiper le vrai async IO (fichiers par cellule, workers) ni
  la simulation hors cellule (P2) ; pas d'état PNJ fin (schedule en
  cours, anim) en v1 — re-dérivé de l'horloge.
- Les guids flecs ne se persistent JAMAIS (World.hpp:16-21) — tout passe
  par `RefId.referenceId`.
