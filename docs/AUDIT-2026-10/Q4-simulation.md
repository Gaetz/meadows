# Q4 — simulation : `data/`, `world/` (hors terrain), `gameplay/`, `script/`, `quest/`

> Vague Q4 de la revue de qualité d'octobre 2026 (`README.md` : méthode, classes
> S/G/D/B/H/P, arbitrages de juillet à ne pas re-proposer). Relecture **intégrale** du
> 2026-10-23 sur HEAD `61efb65` : `data/` (46 fichiers, 5 351 l.), `gameplay/ability` +
> `stats` + `save` (44 f, 5 059 l.), `gameplay/actors` + `combat` + 7 petits dossiers (43 f,
> 4 161 l.), `world/scene` + `worldspace` + `streaming` + `ai` + `dungeon` (42 f, 4 225 l.),
> `world/spirit` + `script/` + `quest/` (22 f, 3 153 l.) — soit ~22 000 lignes, cinq lots en
> parallèle (E1 data, E2 GAS/stats/save, E3 acteurs/combat, E4 world, E5 esprits/script/
> quêtes). `world/terrain/` a été relu en Q1. **Aucun code modifié.** Lignes = `61efb65` ;
> les symboles font foi.

---

## Verdict

**La simulation est l'actif durable, et elle le mérite.** Les cinq lots convergent : `data/`
est « l'unité la plus saine de la série, et de loin » (§5 implémenté tel qu'écrit, resolver en
trois phases, `FieldConflict` porte la valeur, synthèse = plugin ordinaire, 20 fichiers de
tests), `gameplay/actors` + `combat` est « le lot le plus propre » (décisions pures, enums à
un point de transition, `resolveMeleeStrike` pour les deux camps, `kClauseEvaluators`),
`world/` est « sain » (le Spawner est une table, `Perception` applique la règle « une fonction
de transition » au mot, `AnimBridge` route les conditions par l'évaluateur partagé), le cœur
GAS « est sain et verrouillé par 20 tests », `script/` applique §2.8 au mot et `SpiritRules`
impose la règle BotW par la **forme** de sa table. §2.10 passe sans exception dans les cinq
dossiers : zéro include `rhi/render/platform/SDL`. §2.9 passe : aucune écriture d'attribut
hors du pipeline ou des execution calcs sanctionnées. Aucune fonction ne dépasse 150 lignes
sauf `stageDungeonRecords` (397) et `readWater` (130, séquentiel). Les arbitrages de juillet
ont tous tenu dans le code.

Ce qui reste n'est **pas du spaghetti local, c'est une cause racine et ses symptômes**, la
même que Q1-Q3 ont nommée de l'extérieur : **la réflexion n'a ni kind ni métadonnée
`Enum`**. Conséquence mesurée : **19 champs-vocabulaires en `str` + 6 en `i32`** dans les
Forms (12 dans `data/forms`, 7 hors), **~25 vocabulaires fermés** consommés par 5 `parse*` et
26 comparaisons `== "…"` littérales, **chacun écrit trois fois** (le commentaire du Form, le
parseur ou la chaîne de `if`, la liste de l'éditeur `Keywords.cpp` déjà divergente) ; quatre
vocabulaires n'ont même pas d'enum dans la couche qui les déclare (`AiPackageForm.kind`,
`combatStyle`, `FurnitureForm.category`, les noms d'événements hashés sans registre) ; les
défauts silencieux sont partout (`parseStatusType` ignore sa propre table `kRows` et renvoie
`Poison`, `parseOp` renvoie `Add`, `parseStanding` renvoie `Neutral`, une faute de frappe
dans un `.event` TOML = règle qui ne tire jamais). `Perception.hpp:35` porte l'aveu littéral :
`i32 state { 0 }; // AwareState — reflect has no enum kind`. Deux options compatibles avec
l'arbitrage H-a de juillet sont instruites (Q4-7, avec Q5-3 côté `engine/reflect`) ; la
décision est au dev (§2.3, §5).

Second axe, **les tables à moitié écrites** : `DamageType` a sept chaînes de traits dans
cinq fichiers ; `tickBuildup` porte des `if` par type **dans** une boucle de table ;
`Injuries.cpp` est sept switches plus trois `EffectForm` construits en C++ ; la matrice des
sorts est écrite trois fois (sim, scène, test) ; `kClauseEvaluators` est la table modèle mais
`conditionSummary` est un second if-chain de 8 dans le même fichier ; `GroundClass` re-liste
cinq classes de sol que la table data compile déjà ; le cœur GAS connaît le contenu
(health/energy/essence ×2, `attr("energy")` spécial, triade onyx/amber/garnet en sept sites).

Troisième axe, **la persistance contourne la réflexion là où elle est le plus utile** : le
bundle acteur est en trois listes incomplètes (Spawner 9, scène 6, save 10) alors que le pont
`ecs::World::reflectedComponent` existe sans client (Q5-5) ; `ActiveEffect` n'est pas
réfléchi et est recopié champ à champ dans les deux sens ; `SavedStatsForm` est la troisième
écriture de 55 champs de dix composants ; `ScriptVars` reste « serialization deferred » alors
que §2.8 promet la persistance gratuite ; les cellules de feu sont sérialisées en texte
`"%.2f %.2f %u …;"` dans un champ `str` ; deux stagers de records identiques (`MapRecords`,
`DungeonRecords`) déréférencent `findField(field)->id` sans test, et le dépôt compte 14 de ces
déréférencements nus.

Les positifs sont la **moitié du rapport** et doivent servir de modèles aux corrections de
Q1-Q3 : `Resolver::resolve`, `diffToRecord`, `Synthesis`, `CsvImport`, `EditSession` ;
`kRows` + `static_assert`, `DerivedStatRegistry`, `applyBuildupResult` ; `resolveMeleeStrike`,
`setSwingPhase`, `updateLifeState`, `decidePlayerAction`, `kClauseEvaluators`, les sections
« Reused systems (§2.11) » de `Followers.hpp` ; `Spawner` (table catégorie → fonction),
`AnimBridge::GraphForms`, `Perception`, `TriggerSystem`, `CellStreamer` ; `SpiritRules`,
`compileSpell`, `DialogueRunner`, `Vm` (sandbox, `ScriptSelf` qui refuse l'écriture
d'attribut), `advanceTreeFire`.

---

## Contrôles d'invariants

| Invariant | Verdict | Remarque |
|---|---|---|
| §2.10 : `data/ world/ gameplay/ script/ quest/` sans `rhi/render/platform/SDL/GL/ImGui` | **PASS** (5/5) | grep vide dans les cinq dossiers ; `toml++` en PRIVATE ; le namespace `render::terrain` est lexical (Q1-H) |
| §4 : `engine/*` n'inclut jamais `data/*` | PASS | grep vide |
| §2.2 : Forms jamais mutées au runtime | PASS (jeu) / trappe outil | `getMutable` ×2 dans `DungeonRecords.cpp` (générateur, « TOOL-ONLY ») |
| §2.3 : réflexion = clé de voûte | PASS (forme) / **lacune** (fond) | tout Form et composant réfléchi, sauf `ActiveEffect` (Q4-4) ; pas de kind Enum (Q4-7) ; `packFireCells` = sérialisation ad hoc (Q4-3) |
| §2.4 / §5 : une couche pour mods ET saves, patches par champ | PASS | `resolve(vector<const Plugin*>)` ne distingue rien ; saves = records via `diffToRecord` ; `FieldConflict` porte la valeur (§5.1 rempli) |
| §2.5 : identité par GUID | PASS | `FormHandle` non persisté ; `cellGuidFor`, prefabs, `csvRowGuid`, dungeon `derived()` déterministes |
| §2.7 : Spawner par catégorie, en un endroit, valeurs par réflexion | PASS (world) / **partiel** (acteur) | table `FormCategory → SpawnFn` ; le bundle acteur en trois listes (Q4-2) |
| §2.8 : état script en composants réfléchis, `self` = handle | PASS / **écart** | `ScriptVars` réfléchi mais non sérialisé (Q4-4) |
| §2.9 : attributs mutés par effets ou execution calcs sanctionnées | **PASS** | Option A tenue ; `ScriptSelf::newindex` lève sur un attribut ; init/spawn/level-up commentés ; aucune écriture nouvelle |
| §6.1 : factions = tags + table de relations | PASS | `ActorTagForm`, `FactionRelationForm`, `Bounty` = la « thin component » sanctionnée |
| §2.11 : réutiliser avant de construire | PASS | `Dialogue` sur `childrenOf` + `conditionsPass` ; `AnimBridge` sur `ConditionForm` ; `Followers.hpp` nomme ses systèmes réutilisés par section |
| §8 déterminisme | PASS | `core::Rng` seul ; captures triées ; `Floaters` sans RNG ; sandbox Lua sans `math.random` |
| Règle dev « enum over nested ifs » | PASS (modèle) / écarts T-3 | `SwingPhase`, `AwareState`, `MoveMode`, `TreeFirePhase`, `LifeState` ; écarts : `Perception.state` i32, `followerStance` f32, `EditOp` union taguée, blend en 3 bools |
| Index parent | **confirmé légitime** | `FormDatabase` n'indexe que le champ littéral `"parent"` ; les quêtes ont `state`/`branch` → flag `ParentLink` (Q4-18) |
| Politique de commentaires 2026-07-22 | PASS (data 10, GAS 16, actors 5, world ~18, spirit 11 tags, script/quest 0) | ~60 écarts sur 22 000 l. ; 7 « HOW TO FILL » à trancher |
| Arbitrages de juillet | respectés ; **deux ré-ouverts avec preuve** | H-a, U6 Option A, U7-5/7/8, U8-6/9 tenus ; **H-b** (`cloneFields`) : 2e copie identique dans `DungeonRecords` ; **U8-1** (scan manuel) : clone survivant dans `Factions.cpp:38` et `WorldModel.cpp:16` |

---

## Passe 1 — fichier par fichier

### `data/` (lot E1) — la plus saine de la série

| fichier | lignes | rôle | verdict | findings |
|---|---|---|---|---|
| `forms/Form.hpp`, `FormDatabase`, `FormQuery`, `FormTypeRegistry` | 39-174 | `Form`, propriétaire des forms résolus, index guid/type/parent, requêtes typées | **propre** | Q4-18 (`parent` littéral) |
| `forms/CoreForms` | 317 | Weapon/Armor/Consumable/MiscItem/Actor ; `weight`/`goldValue`/`unremovable` déclarés 4× | propre | Q4-7, Q4-15 |
| `forms/LandscapeForms.hpp` | **1 220** | 7 Forms de tuning (~180 + ~165 champs), 4 « defaults MUST match » dont 1 seul testé, 2 champs morts avec une date | à surveiller | Q4-20, Q4-31 |
| `forms/LandscapeForms.cpp` | 97 | 5 résolveurs de singleton copiés | copie | Q4-24 |
| `forms/Visual`, `Anim`, `Audio`, `Spirit`, `Ui`, `Loc` | 43-218 | 9 vocabulaires `str` dans `SpiritForms`, 3 dans `VisualForms`, `triad` i32 | propre | Q4-7 |
| `plugins/Resolver` | 265 | §5 en 180 l., trois phases nommées | **propre — modèle** | Q4-25 (`rankOf` ×2) |
| `plugins/RecordDiff`, `Synthesis`, `Validate`, `CsvImport`, `PluginConfig`, `TomlWriter` | 50-232 | `diffToRecord`, §5.1, lint par réflexion, import déterministe | **propre — modèles** | Q4-19 (doublons `editorId`) |
| `plugins/PluginLoader` | 278 | TOML → `Plugin`, `convertValue` (Reader H-a) | propre | Q4-27 (B) |
| `plugins/BinaryFormat` | 351 | codec cuit via `visit` — **c'est** le `BinaryArchive` de T-5 | propre | Q4-28 (B) |
| `plugins/EditSession` | 423 | drafts, undo/redo, export via `diffToRecord` ; `copyFields` ; `EditOp` union taguée | propre | Q4-19, Q4-21 |
| `editor/EditorLayouts`, `GraphLayout` | 335 | side-store hors plugins, layout déterministe | **propre** | — |

Fonctions > 80 l. : `parsePluginToml` 140, `importCsv` 120, `resolve` 180 — toutes des
séquences lisibles. Aucune chaîne `if` de cas particuliers hors les switches par `FieldKind`.

### `gameplay/ability`, `stats`, `save` (lot E2)

| fichier | lignes | rôle | verdict | findings |
|---|---|---|---|---|
| `ability/GameplayEffects` | 622 | `EffectForm`, `applyEffect` 102 l., tick partagé, `effectWarnings` (3e validateur) | à surveiller | Q4-7, Q4-14, Q4-26 |
| `ability/GameplayAbility`, `AbilitySystem`, `Attributes`, `GameplayTags`, `DerivedStats` | 62-255 | ASC, tags ref-comptés, registre de stats dérivées | **propre — modèles** | Q4-14 |
| `stats/StatusBuildup` | 254 | `kRows` + `static_assert` (modèle) ; `tickBuildup` avec `if` par type ; `parseStatusType` hors table | **à compléter (colonnes)** | Q4-7, Q4-11 |
| `stats/Damage` | 393 | `DamageType`, `applyDamage` 85 l., 5 compte-à-rebours | à surveiller | Q4-9, Q4-13 |
| `stats/EquipmentStats` | 331 | 4 sondes d'items, copie d'`isPhysical`, `isUpgrade` sur slot string | à surveiller | Q4-9, Q4-15 |
| `stats/Injuries` | 271 | 7 switches de traits, 3 `EffectForm` en C++ | à refondre (table ou data) | Q4-12 |
| `stats/CharacterStats` | 253 | `registerCoreDerivedStats` 164 l. = 36 formules (données en C++) | propre | Q4-32 |
| `stats/StatsTuning`, `Resonance*`, `Afflictions`, `Drugs`, `Rest`, `Survival`, `GameClock`, `GameTime`, `Skills` | 26-428 | 105 knobs, cascade d'harmonie, tick game-time | propre | Q4-23, Q4-24 |
| `save/SaveForms` | 343 | `SavedStatsForm` = 55 champs miroirs par nom | à surveiller | Q4-20 |
| `save/SaveState` | 495 | `captureActor` 138 l. (6 blocs), `restoreActiveEffect`, `copyMatchingFields` | à surveiller | Q4-2, Q4-4, Q4-6, Q4-26 (B) |

### `gameplay/actors`, `combat` et les sept petits dossiers (lot E3) — le plus propre

| fichier | lignes | rôle | verdict | findings |
|---|---|---|---|---|
| `actors/Followers.{hpp,cpp}` | 1 100 | 38 fonctions **pures** en 8 sections, aucun état, 14 TU dépendants | propre, **catalogue à scinder** | Q4-5, Q4-19, Q4-23, Q4-25 |
| `actors/CharacterTick` | 284 | pipeline par acteur en 6 phases ; 3 listes des 9 composants GAS | propre | Q4-2, Q4-23 |
| `actors/ActorState`, `CharacterForms`, `FollowerForms`, `Riding`, `Swimming` | 62-265 | stance en `f32` ; `classAttributesAt` 9 lignes à la main | propre / **modèles** (`decideMoveMode`) | Q4-19, Q4-20 |
| `combat/MeleeSwing` | 426 | `SwingPhase` + `setSwingPhase`, segment vs capsule (Ericson) | **modèle** | — |
| `combat/MeleeStrike`, `Combat`, `PlayerAction`, `Projectile`, `CombatAi` | 61-269 | `resolveMeleeStrike`, `updateLifeState`, arbitre pur ; type de coup décidé 2× | **modèles** | Q4-9, Q4-7 |
| `ai/AiForms`, `ScheduleSystem` | 202 | `kind` = un commentaire, **aucun enum** | propre, vocabulaire absent | Q4-16 |
| `condition/Condition` | 237 | `kClauseEvaluators` (8) + `conditionSummary` if-chain (8) | **modèle à compléter** | Q4-8 |
| `cue`, `event`, `faction`, `interaction`, `inventory` | 108-165 | repli hiérarchique, ordre déterministe, §6.1 ; scan manuel (U8-1) ; `addItem` sans contrat | propre | Q4-16, Q4-19, Q4-25, Q4-30 (B) |

Fonctions > 80 l. : **aucune**.

### `world/` hors terrain (lot E4)

| fichier | lignes | rôle | verdict | findings |
|---|---|---|---|---|
| `scene/Spawner` | 371 | table `FormCategory → SpawnFn`, universels par `findField`, prefabs à guids dérivés ; 9 composants acteur | **propre — modèle** | Q4-2, Q4-19, Q4-32 |
| `scene/Components`, `AnimBridge`, `TriggerSystem`, `SpatialIndex`, `Collision`, `KillZ`, `Floaters`, `Movement`, `MapBounds` | 31-315 | 15 composants réfléchis ; UNE mapping Forms → graphe anim ; snapshot-puis-agir | **propre — modèles** | Q4-25 |
| `worldspace/WorldForms.hpp` | 476 | 17 Forms de six domaines dans l'en-tête que tout `world/` inclut ; `FireStateForm.cells` texte | sain, **fourre-tout** | Q4-3, Q4-6, Q4-19 |
| `worldspace/WorldModel`, `FormCategory` | 223+86 | index spatial en ordre de handle, `materializeCell` idempotent ; clone de scan U8-1 | propre | Q4-25 |
| `streaming/CellLoader`, `CellStreamer` | 133+160 | synchrone **par décision**, hystérésis, budget | propre | — |
| `ai/Perception` | 183 | enum + switches plats, `setAwareState` = LA transition ; `i32 state // reflect has no enum kind` | **propre — modèle** | Q4-19 |
| `ai/TerrainNavigator`, `InteriorNavigator`, `Pathfinding`, `GridNavigator`, `Steering` | 31-228 | trois A* sur un squelette ; contrat `Navigator` divergent | propre | Q4-22, Q4-29 (B) |
| `dungeon/DungeonRecords` | 596 | `stageDungeonRecords` **397 l.**, 12 paramètres, 27 `stage.set` à la main ; `Stager` copie de `MapRecords` | **à refondre** | Q4-1, Q4-21, Q4-32 |

### `world/spirit`, `script`, `quest` (lot E5)

| fichier | lignes | rôle | verdict | findings |
|---|---|---|---|---|
| `spirit/Spells` | 220 | `SpellSpec`, 3 parseurs, `compileSpell` ; `spellSupported` = la matrice en `if × switch` | propre, **matrice en `if`** | Q4-7, Q4-10 |
| `spirit/SpiritRules` | 182 | table compilée depuis la data, shifumi dérivé de l'enum, règle BotW imposée par la forme | **propre — modèle** | Q4-7 (`kVerbs` sans `static_assert`) |
| `spirit/SpiritFire` | 360 | `GroundClass` en dur ; `pack/unpackFireCells` texte maison ; `advanceTreeFire` enum + switch | propre / **écarts** | Q4-3, Q4-17 |
| `spirit/SpiritSources`, `SpiritJets`, `WaterReading`, `SpiritWaterMesh` | 149-358 | listes bornées, guids dérivés ; `readWater` 130 l. séquentiel ; balistique ×3, 4 m ×4, `riverAt` 11e copie | propre | Q4-25, Q4-32 |
| `script/Vm` | 666 | sandbox, `ScriptSelf` proxy, coroutines par handle, brains ; bracket `self` ×5 | **propre** | Q4-25 |
| `script/ScriptVars` | 36 | composant réfléchi ; « serialization deferred » | propre / **écart §2.8** | Q4-4 |
| `quest/Quest`, `Dialogue` | 452 + 196 | `QuestIndex`, `onQuestEvent` pur, save par records ; `kind == "Success"` ×2 ; `DialogueRunner` sur `childrenOf` + évaluateur partagé | **propre — modèles** | Q4-7, Q4-18, Q4-25 |

---

## Passe 2 — système par système

### S1. La cause racine : les vocabulaires fermés sont des strings (Q4-7, Q4-8, Q4-16, Q4-17, Q4-19)

**Pattern.** Inventaire des champs-vocabulaires (documentés par un commentaire `a | b | c`) :
`data/forms` 12 (`ArmorForm.slot`, `ConsumableForm.category`, `LightForm.kind/shadowMode`,
`ParticleForm.shape/blend`, `SoundForm.bus`, `AnimTransitionForm.compare`,
`SpellForm.form/trajectory/areaShape/element`, `SpiritForm.channel/fieldKind/damageType`,
`SpiritRuleForm.verb`, `ActorForm.followerCategory`) + hors `data/` 7 (`EffectForm.op/duration`,
`PackageForm.kind`, `ConditionForm.kind`, `FurnitureForm.category`, `SpiritSourceForm.spirit`,
`QuestStateForm.kind`) + entiers-énumérations 6 (`RcTuningForm.technique`, `leafShape`,
`leafStyle`, `SpiritForm.triad`, `SavedQuestForm.status`, `ZoneArchetypeForm.palette/piece`
= Q1-14). Consommateurs : `parseOp` (défaut **Add**), `parseDuration` (défaut **Instant**),
`parseStatusType` (**ignore `kRows`**, défaut **Poison**), `parseCombatMove`, `parseStanding`
(défaut **Neutral**), `parseSpellVerb/Trajectory/Area` (12 `if`), `questStatusOf` écrit 2×,
`lightSourceFrom` (3 `==`), `toEmitterParams` (ternaires + 3 bools), `countPartyMember` et
`canJoinParty` (2 if-chains sur 3 valeurs), `costPolicy` avec `== "energy"` en dur. Les
troisièmes copies : `effectWarnings` (`GameplayEffects.cpp:440-480`), `Keywords.cpp` (Q2-42,
divergée), `ConditionBuilder.cpp`. Quatre vocabulaires **sans enum dans `gameplay/`** :
`AiPackageForm.kind` (parsé dans `game/NpcScheduleController.cpp:109-136`, `"guard"` par
défaut), `FollowerClassForm.combatStyle` (« CombatAi vocabulary » qui n'existe pas, `==
"healer"` ×2 dans `game/`), `FurnitureForm.category` (Q2-17), et les **noms d'événements** :
`eventKind("…")` hashe n'importe quoi, `AffinityRuleForm.event`/`CommentForm.event`/
`QuestTaskForm.event` sont matchés par hash sans registre — une faute de frappe dans un TOML
= règle muette. `GroundClass` (`SpiritFire.hpp:37-40`) re-liste cinq classes que
`SpiritRuleTable.classes` compile depuis `SurfaceMaterialForm` (dont le commentaire en promet
huit) : le feu ne brûlera jamais `wood/bush/metal` quelles que soient les données.

**Mécanisme à étendre.** `kClauseEvaluators` (`Condition.cpp:74-84`, « adding a new kind is
adding an entry ») et `kRows` (`StatusBuildup.cpp`, déjà `static_assert(size == kCount)`) sont
les tables modèles ; `kVerbs` (`SpiritRules.cpp:10-32`) est la bonne forme sans `static_assert`.
Il leur manque le **point d'accroche** qui les relie au champ réfléchi — c'est la décision
T-3, instruite côté réflexion en Q5-3 :

- **(B) `FieldMeta` / `EnumTable` sur `FieldInfo`** — `REFLECT_FIELD_ENUM(slot, kArmorSlots)`
  attache une table de noms (`static_assert` sur `Count`, T-4) au champ dont le kind reste
  `Str` (ou `I32`). **Zéro changement du format TOML, binaire, saves.** Le loader valide le nom
  à la lecture (« unknown value 'bedd' for slot », aujourd'hui silencieux), la grille ImGui tire
  un combo, Lua idem, les 5 parseurs deviennent `meta.enumeration->parse(text)`. Compatible
  H-a (une **donnée** sur le champ, pas une table de corps couplant binaire↔ImGui↔Lua).
- **(A) `FieldKind::Enum` appendu** — `Value` gagne `EnumValue { ordinal, table }`, TOML écrit
  le nom, binaire l'ordinal, les Forms portent l'`enum class` directement ; c'est un changement
  de format (bump binaire, migration `str` → enum) et chaque consommateur `visit` gagne un
  corps (le contrat de `Visit.hpp` : le build casse jusqu'à ce que chacun l'ait).

Les deux lots qui l'ont instruit (E1, F1) recommandent **(B) d'abord**, (A) par-dessus si
nécessaire. Dès que l'un ou l'autre est acté : `kClauseKinds[] { name, eval, summary, fields }`
(Q4-8) remplace `kClauseEvaluators` + `conditionSummary` + la copie `Keywords.cpp` ; `enum
class PackageKind` + `kPackageKinds[]` dans `gameplay/ai`, `enum class CombatStyle` dans
`CombatSituation` (T-12 : la décision pure côté `gameplay::`, `NpcScheduleController` fait un
`switch`) ; `EventBus::knownKinds()` alimenté par les `eventKind("…")` C++ + un `cooker
validate` qui vérifie les `.event` des Forms (Q4-16) ; `fuelFromWeights` indexé par
`classIndex` de la table data, poids construits depuis `kSplatLayers[].family` (Q4-17, avec
Q3-21).

**Risque.** Nul sur le comportement (mêmes strings acceptées) ; (B) nul sur disque ; (A) recook
des plugins binaires. Tests : `ConditionTest`, `GameplayEffectsTest`, `StatusBuildupTest`,
`CombatAiTest`, `FactionsTest`, `SpellsTest`, `SpiritFireTest`.

### S2. Les tables à moitié écrites du GAS et du combat (Q4-9, Q4-10, Q4-11, Q4-12, Q4-13, Q4-14, Q4-15)

**Pattern.**
- **`DamageType`** (Q4-9) : `Damage.cpp:10 damageTypeName` (switch 12), `:49 isPhysical`,
  `:56 mitigationPercent` (switch → nom d'attribut), `killOutright:35`, `EquipmentStats.cpp:52-61
  weaponDamageEvent`, `:77-79` (copie d'`isPhysical`), `:9-21 armorModifiers` (12 champs → 12
  `attr`), `CharacterStats.cpp:58-86` (12 formules resist/armor), `MeleeStrike.cpp:28-31`
  (edged/blunt) — sept chaînes dans cinq fichiers, 43 sites `DamageType::` dans `gameplay/`.
  Et le « type du coup » est décidé **deux fois différemment** : `maybeRollInjury` (type
  dominant par somme) vs `resolveStrikeDamage` (premier canal) — une arme Blunt+Slash blesse
  en Bruise et joue `Cue.Hit.Slash`.
- **Matrice des sorts** (Q4-10) : `Spells.cpp:91-161 spellSupported` = 4 blocs `if (element)` ×
  `switch (verb)` × tests trajectoire/channeled (15 cellules vraies sur 200) ;
  `LandscapeScene.cpp:5160-5362` re-dérive **les mêmes cellules** en `PendingSpiritAction::Mode`
  (Q2-16) ; `SpellsTest.cpp:98-132` les énumère une troisième fois. Le prochain esprit (Foudre)
  = trois fichiers.
- **`tickBuildup`** (Q4-11) : `StatusBuildup.cpp:70-83` trois branches DoT de même forme (base ×
  (1 − vitality) × dt, source flat / `maxHealth` % / `maxEssence` %), `:95-113` trois
  comportements de déclenchement, `:134 « Add one line here per status »`, `BuildupTickResult`
  = 3 floats + 4 bools nommés — des `if` par type **dans** une boucle sur `kRows`.
- **`Injuries.cpp`** (Q4-12) : `resonancePenalty:18`, `attributeMalus:33` (switch × switch ×
  tableaux), `speedMalusPercent:73`, `maxSeverity:92`, `recoveryHours:94`,
  `injuryBaseChance:165` ; `applyInjuryEffect:103-138` construit trois `EffectForm` littéraux
  (`"onyx"`, malus, `×speed`) ; `Survival.cpp:33-39` idem.
- **Cinq compte-à-rebours « timer + tag »** (Q4-13) : `Damage.cpp:163-235` (`updateStagger`,
  `updateParalysis`, `updateCritWindow`, `updateShaken`, `updateDowned`), déclencheurs qui posent
  timer ET tag en deux endroits (`applyDamage:138-156`, `GameTime.cpp:60-71`), appelés en ligne
  par `CharacterTick.cpp:52-56`. **Extension GAS vérifiée** : directe pour 3 des 5 (tag pur,
  `expireEffect` retire le tag), pas pour `critWindow` (refill de posture à l'expiration) ni
  `downed` (retourne bool), et `CombatState.*Seconds` sont persistés **par nom** → changer le
  format de save.
- **Le cœur connaît le contenu** (Q4-14) : paires vital/max écrites 2× (`clampBaseVitals:86`,
  `clampVitalsCurrent:150`), `attrId == attr("energy")` dans `applyEffect:257` et `canAfford`
  (`GameplayAbility.cpp:28`), `kEnergyRegenDelay = 1.0f` « → StatsTuningForm later », 125
  `attr("…")` (35 registrations légitimes), 34 littéraux `"State.*"` (12 distincts), 25
  `"Status.*"`.
- **Items sans base commune** (Q4-15) : `EquipmentStats.cpp:103-116 itemWeight`, `:172-185
  itemUnremovable`, `:188-202 gearPower`, `:205-234 isUpgrade` sondent quatre types ;
  `CoreForms.hpp` déclare `weight/goldValue/unremovable` quatre fois. **`TypeInfo::isA` et
  `parent` existent** (`Reflect.hpp:137, 152`, 10 sites) : le « parent réfléchi » que le plan
  croyait à créer est déjà là.

**Mécanisme à étendre.** `kRows` + `static_assert` (T-4), `DerivedStatRegistry` (formules =
données), `applyBuildupResult` (U6-F2, l'unique écrivain), `kVerbs`, `REFLECT_BEGIN(T, Parent)`.

**Croquis.** `constexpr DamageTypeTraits kDamageTypes[12] { name, physical, mitigationAttr,
sourceAttribute, f32 WeaponForm::* attack, f32 ArmorForm::* mitigation, InjuryClass }` +
`dominantDamageType(const DamageEvent&)` : `weaponDamageEvent`, `armorModifiers`,
`mitigationPercent`, les 12 resist* et `MeleeStrike` deviennent des boucles ; un 13e type =
une ligne. `constexpr SpellCell kSpellMatrix[] { element, verb, trajectory, channeled, Mode }`
dans `world/spirit/`, `spellSupported = findCell != nullptr`, la scène lit `cell->mode`, le
test parcourt la table (le `Mode` descend de `game/` comme id de handler, T-12 ; **même brique
que Q2-16**). Colonnes sur `TypeRow` : `name`, `OnTrigger { Reset, HoldAndTag }`, `DotSource {
None, Flat, MaxHealthPct, MaxEssencePct }`, `f32 StatsTuningForm::* dotRate`, `regenAttr`,
`regenMult` ; `BuildupTickResult { array<f32, kCount> dot; bitset triggered }`. `Injuries` : (a)
`constexpr InjuryTraits kInjuries[3]` bit-exact, ou (b) `InjuryEffectForm` child-records
(blessures moddables comme les afflictions, ouvre STATS-CONFORMANCE #18) — **décision T-11** ;
dans les deux cas `makeInternalEffect(attr, op, mag, tag)` remplace les 4 littéraux. Timers :
`constexpr TimedState kTimedStates[] { f32 CombatState::* timer, tag, OnExpire }`,
`grantTimedState()` seul écrivain timer+tag, `tickTimedStates()` — bit-exact d'abord, la
migration GAS = décision dev (§6 « stagger est un effet »). `constexpr VitalPair kVitals[3] {
value, max, spendPausesRegen, costPolicyDefault }` dans `Attributes.hpp` ; `namespace tags {
constexpr string_view Dead = "State.Dead" … }` à côté de `registerCharacterRuntimeTags` (une
faute de frappe = erreur de compilation). `struct ItemForm : Form { weight, goldValue,
unremovable }` parent des quatre, `findAs<ItemForm>` par `isA` — save/TOML inchangés (mêmes
noms de champs).

**Risque.** Bit-exact partout (mêmes formules, même ordre) ; `TypedDamageTest`,
`EquipmentStatsTest`, `OffensiveStatsTest`, `StatusBuildupTest` (14 Ko), `InjuriesTest`,
`SpellsTest`, `MeleeStrikeTest`. Changement visible seulement pour les armes multi-canaux
(Q4-9 type dominant) et le format `base.toml` si (b) pour les blessures.

### S3. La persistance qui contourne la réflexion (Q4-2, Q4-3, Q4-4, Q4-6, Q4-20, Q4-26)

**Pattern.**
- **Le bundle acteur en trois listes** (Q4-2, T-13 **précisé**) : le 9-set GAS est **cohérent**
  (Spawner `Spawner.cpp:149-160` = `tickCharacter` ×3 = `initializeActorStats` =
  `gameTimeArgsFor`) — nuance au plan « ×7 divergent ». La divergence porte sur les **six
  composants non-GAS** (`MeleeSwing`, `Inventory`, `Equipment`, `Bounty`, `VendorState`,
  `FollowerState`) attachés par la scène (`LandscapeScene.cpp:4340-4366 finalizeActorSpawn`,
  « present BEFORE the saved-state apply so the name-matched fields land » — la liste existe
  **parce que** le save en a besoin), par `CombatArenaScene.cpp:218`, à la demande par
  `InteractionController`, `UiRouter` ×3, `FollowerSocial` ; le save en connaît 10
  (`SaveState.cpp:135-144`, `:299-308`), sans `ResonanceDecays` (non réfléchi, « transient » par
  en-tête — un chargement coupe net les décroissances en cours, à instruire). Et le pont
  `ecs::World::reflectedComponent` (« drives generic component serialization for saves ») a
  **zéro appelant** hors test (Q5-5).
- **`ActiveEffect` non réfléchi** (Q4-4) : POD plat recopié 12 champs × 2 sens
  (`SaveState.cpp:70-81`, `:101-113`) ; `AbilitySystem.hpp:49` dit « Not reflected — its members
  (containers) » : vrai pour l'ASC, faux pour `ActiveEffect`. **Bug latent prouvé** (Q4-26) :
  `applyEffect` donne le même `effectId` aux lignes primaire et `attribute2` (`:277, :312`,
  « removeEffectById must take both lines out »), `SavedEffectForm` ne porte pas l'id,
  `restoreActiveEffect:82` alloue `nextEffectId++` **par ligne** → après chargement une paire a
  deux ids et `removeEffectById` n'en retire qu'une. Latent : le seul détenteur d'id
  (`Npc.furnitureEffectId`) est runtime et son effet est sans `attribute2`.
- **`SavedStatsForm`** (Q4-20) : 55 champs miroirs par nom de dix composants ; un champ
  ajouté à un composant et oublié ici n'est **pas sauvé, silencieusement** (`maxHealthOverride`
  absent, re-semé au spawn donc inoffensif ; `downedSeconds`, `level`, `followerStance` ajoutés
  après coup). `captureActor` = six blocs de child-rows quasi identiques (Q4-6).
- **`ScriptVars`** (Q4-4) : « serialization is deferred (the map is a container — same story as
  inventories/active effects) » ; §2.8 promet « serializes/diffs through the §5 patch layer for
  free » ; un `self.counter = 3` est perdu au rechargement.
- **`packFireCells`** (Q4-3) : `SpiritFire.cpp:176-241` sérialise les cellules en texte `"%.2f
  %.2f %u %.3f %.3f %.3f;"` dans `FireStateForm.cells` (`str`), relues au `sscanf`, cap 65 536 ;
  `WorldForms.hpp` pose quatre fois la convention inverse (« grids never live in Forms —
  reflection stays flat » → asset). Un patch de mod ne peut pas cibler une cellule, le diff §5
  voit un blob, `%.2f` arrondit x/z.

**Mécanisme à étendre.** `REFLECT_BEGIN` sur `ActiveEffect` ; `copyMatchingFields` (gardé en
juillet) ; `reflectedComponent` + `world.handle().each(entity, …)` ; le patron child-record
(`SavedQuestTaskForm` : une ligne = un record, guid dérivé) ; le `BinaryFormat` codec réfléchi
(T-5) pour un blob binaire réfléchi si 65 k records est trop.

**Croquis.** `gameplay::attachActorKit(entity)` = les 9 + les 6, appelé par `spawnActor`
(§2.7), `finalizeActorSpawn` (qui garde ses initialisations), les bancs ; le save itère la
**même liste** `kActorKit[]` de `TypeInfo*` → `componentToSaved` générique au lieu de 10 lignes
×2 — ou, mieux, le pont `reflectedComponent` fait le tour des composants de l'entité (plus de
liste du tout). `SavedEffectForm.effectId` (u32, défaut 0 = ancien save → id frais), restore
par id d'origine (map ancien→nouveau par acteur), `ActiveEffect` réfléchi → une ligne
`copyMatchingFields` par sens. Test headless « tout champ réfléchi d'un composant persisté
existe sur `SavedStatsForm` sauf exclusion explicite » puis, à terme, `SavedStatsForm`
**généré** depuis les `TypeInfo` (T-1, décision dev : touche la clé de voûte).
`captureRows<Row>(records, ns, parent, range, keyOf, fill)` ×6. `ScriptVars` et les cellules
de feu : un **kind conteneur réfléchi** (`map<str, Value>`, `vector<u8>` + codec) ou des
child-records — **décision commune avec T-5**.

**Risque.** Format de save pour `effectId` (compatible : défaut 0) et pour le feu/ScriptVars
(les saves de dev ne sont pas protégées) ; `SaveStateTest`, `SaveFormsTest`, `FollowersTest`,
`CellDeltaTest`, `SpiritFireTest` round-trip.

### S4. Les stagers, les index et les résolveurs : `data/` a le mécanisme, les couches au-dessus le recopient (Q4-1, Q4-21, Q4-18, Q4-24, Q4-25)

**Pattern.** `DungeonRecords.cpp:49-119 Stager` et `MapRecords.cpp:25-46 Stager` (Q1-38) :
`ensure` et `set` identiques, `set` déréférence `type.findField(field)->id` sans test (une
faute de frappe = crash à l'Accept) ; le dépôt compte **14** `findField("…")->id` nus
(`SaveGame.cpp` ×9, `TerrainSculptTool.cpp` ×3, `SaveState.cpp:150`, les deux stagers) ;
`LevelEditor::fieldId` est le null-safe. `Stager::live()` est la **2e copie identique** de
`copyFields` (`EditSession.cpp:13-17`) — l'arbitrage H-b de juillet (« un appelant chacun ») ne
tient plus pour le clone même-type ; `copyMatchingFields` reste l'opération distincte.
`stageDungeonRecords` (397 l., 12 paramètres) écrit chaque record **deux fois** : struct puis 27
`stage.set` champ par champ, alors que `live()` copie déjà par réflexion — l'asymétrie est le
piège (un champ posé sur la struct arrive en live mais pas à l'export). `FormDatabase.cpp:31`
n'indexe que le champ littéral `"parent"` → `buildQuestIndex` (`Quest.cpp:157-170`) reconstruit
trois index privés parce que les quêtes ont `state`/`branch` (deux niveaux de parenté, une
bonne raison) ; quatre champs morts gardés « pour que les records résolvent » par de la prose
(avec une date `2026-07-30` en commentaire). Six résolveurs de singleton identiques
(`LandscapeForms.cpp:44-83` ×5, `StatsTuning.cpp:55`). Scan manuel `for value <= forms.count()`
(U8-1 ✅ en juillet) survivant dans `Factions.cpp:38-44` et `WorldModel.cpp:16`.

**Mécanisme à étendre.** `EditSession` (le draft), `FieldFlags` (un bit `Transient`, 9 usages,
`REFLECT_FIELD_FLAGS` existe), `FormQuery.hpp`, `data::forEach<T>`.

**Croquis.** `data::RecordStager` dans `data/plugins/` (`ensure`, `set` avec
`ENGINE_ASSERT_MSG` sur le champ, `stageAll(form)` = `forEachField → setField`, `live(form)`
optionnel avec callback d'indexation pour `WorldModel`) ; `reflect::copyFields(type, src, dst)`
public (6 lignes) appelé par `EditSession` et `live()` ; `stageDungeonRecords` → `struct
DungeonStageInputs` + huit fonctions nommées (`stageWorldspace`, `stageCaverns`, `stageTorches`,
`stageNav`, `stageDoors`, `stageKitAnchors`, `stagePatrols`, `disableLeftovers`), `stage.record(
form)` au lieu des 27 `set`. `FieldFlags::ParentLink` posé par `REFLECT_FIELD_FLAGS(state,
ParentLink)` → `indexForm` bucket tout champ Guid flaggé, `childrenOf<T>(db, parent, fieldId)`,
`buildQuestIndex` tombe ; `FieldFlags::Deprecated` → le loader loggue une fois, la grille
masque, `diffToRecord` n'émet jamais, `Validate` compte (aucun effet disque : les flags ne sont
pas persistés). `template <class T> resolveSingleton(db, guid)` + `kSingletonGuids`.
`data::forEach<FactionRelationForm>`.

**Risque.** Nul (mêmes guids, mêmes valeurs ; export comparé octet à octet, `DungeonNavTest`,
`FormIndexTest`, `FactionsTest`). Ferme Q1-38 et les 14 déréférencements nus en une brique.

### S5. Les trois A* et le contrat nav (Q4-22, Q4-29)

**Pattern.** `Pathfinding.cpp:28-80` (4-conn entier, Manhattan), `TerrainNavigator.cpp:56-170`
(8-conn, grille paresseuse, euclidien, acceptation, décimation), `InteriorNavigator.cpp:21-164`
(nœuds = niveaux CSR, surcoût mur) écrivent le même squelette : `priority_queue<OpenNode>` +
`operator>`, `gScore`, `cameFrom`, entrée périmée, `heuristic`, boucle de voisins `tentative <
g`, walk-back inversé. Diffèrent : type de nœud, voisinage, coût d'arête, arrivée, budget.
Le contrat `nav::Navigator` (`Nav.hpp:26-31` : `acceptanceRadius`, « from → to inclusive »)
n'est tenu que par `TerrainNavigator` : `InteriorNavigator` ignore l'acceptation,
`GridNavigator` l'ignore **et** ne renvoie ni `from` ni `to` — un consommateur commun (packages
IA, voyage des schedules) qui vise à 2 m d'un point échoue en intérieur là où l'extérieur
réussit (Q4-29). Nuance : `NavGrid` (`engine/dungeon`) est une donnée, pas un 4e A* ; le
doublon côté dungeon est `navValidates` vs `InteriorNavigator` (Q5-12).

**Croquis.** `ai::astar<Node>(start, isGoal, forEachNeighbor(node, fn(next, cost)), heuristic,
budget)` header-only dans `engine/nav/` ; les trois navigators = trois adaptateurs de ~40 l. ;
le contrat écrit dans `Nav.hpp` (acceptation obligatoire, endpoints exacts) et honoré par
l'`astar` partagé. Bit-identique si ordre d'expansion et tie-breaking conservés
(`TerrainNavigatorTest`, `DungeonNavTest`, `MovementTest`).

### S6. Structure et hygiène (Q4-5, Q4-6, Q4-19, Q4-23, Q4-25, Q4-31, Q4-32)

`Followers.hpp` (462 l.) est un **catalogue**, pas un objet-dieu : 38 fonctions pures en 8
sections, aucun état, chaque section ouvre par « Reused systems (§2.11) : … » ; son coût est le
poids d'include (14 TU dont 9 de `game/`, chacun n'utilisant qu'une section) et `FollowersTest`
74,6 Ko → cinq en-têtes par domaine (`FollowerFollow`, `FollowerLife`, `FollowerAffinity`,
`FollowerProgression`, `FollowerParty`), mêmes symboles (Q4-5). `WorldForms.hpp` (476 l.) :
17 Forms de six domaines inclus par tout `world/` → cinq en-têtes (Q4-6). La triade
onyx/amber/garnet est écrite en **sept sites** de `gameplay/`, le pré-scan « decay on expiry »
**verbatim** entre `CharacterTick.cpp:58-72` et `GameTime.cpp:140-155`, `foldMul` ×5, « find
puis has » ×8, « find puis add » ×5 → `kResonanceTriad[]` + `isResonanceAttribute`,
`collectExpiringDecays()`, `StatModifiers::multiply/add`, `hasTag/addTag` à côté de
`syncStateTag` (Q4-23). Les petits vocabulaires et unions (Q4-19) : `EditOp` taguée par « quel
champ est rempli » → `EditOpKind` ; `followerStance` en `f32` décodé `== 1.0f` (funnel correct
sur un stockage imposé par la sweep) ; `Perception.state` i32 ; `LightForm.kind`, `Marker.kind`
(« spawn », « idle » sans consommateur) ; `editorId` dupliqué : `findByEditorId` garde le
**premier**, `TextTable` le **dernier**, aucun lint. Les petits doublons (Q4-25) : `enterState`
×2, rayon du disque noyau `4.0f` ×4 (la vérité vit dans `WaterSim` qui ne l'exporte pas),
balistique ×3, `riverAt` = 11e copie du prédicat « quel corps d'eau couvre (x, z) », π/τ
littéraux ×7 (pas de `core::kPi/kTau`), bracket `self`/`target` Lua ×5 → `SelfScope` RAII,
`callBrain` 9 champs à la main → `tableFromReflected`, `dispatchTriggerEvent` ×2, clé XZ
`(u32(x) << 32) | u32(z)` **×13 dans le dépôt** (une avec `^`) + `floor(v / size)` ×4 →
`core::packXZ` (Q3-39), matcher « événement + filterTag + partie » ×3, `rankOf` ×2.
Hygiène (Q4-31, Q4-32) : ~60 commentaires hors politique sur 22 000 l. (data 10 dont une
**date**, GAS 16, actors 5 + 7 « HOW TO FILL » à trancher, world ~18, spirit 11 tags « Chantier
ESPRITS » en en-tête, script/quest 0) ; **trois champs de `SpiritForm` que personne ne lit**
(`triad`, `channel`, `fieldKind`) ; `bleedBurstDamage` « OBSOLETE » toujours réfléchi,
`armorRating` sans lecteur ; constantes de formule sans `[cpp-tuning]` (U4-7) ; `editorId[96]` +
`snprintf` ×12 et la borne magique `64` du sweep des leviers dans `DungeonRecords` ; sept
`static_cast` de catégorie sans assert dans `Spawner` ; `SpiritJetList::tick` qui perd
l'émetteur sans `stopped`.

---

## Bugs trouvés au passage (classe B) — aucun corrigé

| id | sév | symbole (fichier) | effet | correction | compat |
|---|---|---|---|---|---|
| **Q4-26** | basse (latent, **prouvé**) | `restoreActiveEffect` (`SaveState.cpp:82`) vs `applyEffect` (`GameplayEffects.cpp:277, 312`) | la ligne `attribute2` partage l'`effectId` de la primaire ; `SavedEffectForm` ne le porte pas et le restore alloue `nextEffectId++` par ligne → après chargement `removeEffectById` n'en retire qu'une (l'invariant écrit dans le code est faux dès le premier load) ; latent car le seul détenteur d'id est runtime et sans `attribute2` | `SavedEffectForm.effectId` (défaut 0 = id frais), remap par acteur ; avec `ActiveEffect` réfléchi | save compatible |
| Q4-27 | basse | `convertValue` (`PluginLoader.cpp:39-47`) | `I32` ← `static_cast` d'un `i64` sans test, `U32` teste `>= 0` mais pas `<= UINT32_MAX` : `goldValue = 5000000000` dans un mod devient négatif **sans warning** (le reste du loader loggue chaque cellule rejetée) | `std::in_range<i32>` sinon `nullopt` ; vérifier `valueFromString` (Q5-18) | nul |
| Q4-28 | basse (latent) | `readPluginBinary` (`BinaryFormat.cpp:220-297`) | ne pose jamais `Plugin::baseDir` (« resolves relative asset paths ») ; seul `cooker uncook` le consomme aujourd'hui | `readPluginBinaryFile(path)` symétrique de `loadPluginFile`, ou documenter « le cuit ne porte pas d'assets » | nul |
| Q4-29 | basse (latent) | `nav::Navigator` (`Nav.hpp:26-31`) vs `InteriorNavigator.cpp:94`, `GridNavigator.cpp:21-25` | contrat `acceptanceRadius` + « from → to inclusive » tenu par 1 adaptateur sur 3 : un consommateur commun échoue en intérieur là où l'extérieur réussit | contrat écrit, deux adaptateurs alignés (ou l'`astar` partagé, Q4-22) | nul |
| Q4-30 | basse (latent) | `addItem` / `removeItem` (`Inventory.cpp:12-20`) | `count <= 0` crée ou décrémente une pile (pile négative possible), `removeItem(…, 0)` renvoie `true` ; `applyLoadout` et `transferAllItems` se protègent chacun = preuve du contrat manquant ; 15 appelants | `ENGINE_ASSERT(count > 0)` ou early-return + contrat dans le header | nul |

Notes hors classe B : `ResonanceDecays` n'est pas réfléchi ni sauvé (« transient » par
en-tête) → un chargement coupe net les décroissances en cours — **décision de design à
confirmer** (Q4-2) ; `startCoroutine:415` / `tickCoroutines:470` `result.get<double>(0)` sur un
`yield` non numérique (un script qui `coroutine.yield("x")` hors `wait`) ; `SpiritJetList::tick`
sans `stopped` perd le handle d'émetteur (aucun appelant sans `stopped`).

---

## Findings

Format : `**Qn-k** [classe / sévérité / effort]` — constat (citations) ; *Action* ; risque.

### S — structure

**Q4-1** [S / moyenne / M] — `stageDungeonRecords` (`DungeonRecords.cpp:132-528`) : 397 l., 12 paramètres, 8 phases, **chaque record écrit deux fois** (struct puis 27 `stage.set` champ par champ alors que `Stager::live()` copie par réflexion). *Action* : `DungeonStageInputs` + huit fonctions nommées + `stage.record(form)`. Export comparé octet à octet.

**Q4-2** [S / moyenne / S — T-13] — le bundle acteur en trois listes : Spawner 9 GAS (`Spawner.cpp:149-160`, cohérent avec `tickCharacter` ×3), scène 6 non-GAS (`LandscapeScene.cpp:4340-4366`, « present BEFORE the saved-state apply »), save 10 (`SaveState.cpp:135-144`, `:299-308`) sans `ResonanceDecays` ; bancs ad hoc ×4 ; le pont `reflectedComponent` sans client (Q5-5). *Action* : `gameplay::attachActorKit(entity)` appelé par `spawnActor`, `finalizeActorSpawn`, les bancs ; le save itère la même liste ou le pont réfléchi. `SaveStateTest`, `FollowersTest`, `CellDeltaTest`.

**Q4-3** [S / moyenne / M — décision T-5] — `packFireCells/unpackFireCells` (`SpiritFire.cpp:176-241`) : grille sérialisée en texte dans `FireStateForm.cells` (`str`), à rebours de la convention « grids never live in Forms » posée 4× dans `WorldForms.hpp`. *Action* : child-records par cellule (patron `SavedQuestTaskForm`) ou blob binaire réfléchi (`BinaryFormat`, T-5). Format de save ; `SpiritFireTest`.

**Q4-4** [S / moyenne / M — décision] — `ActiveEffect` non réfléchi recopié 12 champs × 2 sens (`SaveState.cpp:70-81, 101-113`) ; `ScriptVars` « serialization deferred » contre la promesse §2.8 (`ScriptVars.hpp:13-16`). *Action* : `REFLECT_BEGIN(ActiveEffect)` + `copyMatchingFields` ; un kind conteneur réfléchi ou `SavedScriptVarForm` child-records — décision commune avec Q4-3/T-5.

**Q4-5** [S / moyenne / M] — `Followers.hpp` 462 l. : catalogue de 38 fonctions pures en 8 sections, 14 TU dépendants (9 de `game/`), `FollowersTest` 74,6 Ko. *Action* : cinq en-têtes par domaine, mêmes symboles, `Followers.hpp` agrégateur transitoire ; le test se scinde de même. Nul.

**Q4-6** [S / basse / S] — `captureActor` (`SaveState.cpp:158-262`) : six blocs de child-rows quasi identiques → `captureRows<Row>()` ; `WorldForms.hpp` (476 l.) : 17 Forms de six domaines inclus par tout `world/` → `WorldForms`/`TerrainForms`/`WaterForms`/`BiomeForms`/`SpiritForms`, `registerWorldFormTypes` reste la source unique. Nul.

### G — généraliser

**Q4-7** [G / haute / M — **décision dev §2.3 (T-3)**] — 19 champs-vocabulaires `str` + 6 `i32`, ~25 vocabulaires fermés, 5 `parse*` + 26 `== "…"`, chacun écrit 3× (Form, parseur, `Keywords.cpp`), défauts silencieux (`parseStatusType` ignore `kRows` → `Poison`, `parseOp` → `Add`, `parseStanding` → `Neutral`) ; `Perception.hpp:35 // reflect has no enum kind`. *Action* : **(B)** `FieldMeta`/`EnumTable` sur `FieldInfo` (zéro changement disque, loader valide, grille → combo) d'abord ; **(A)** `FieldKind::Enum` appendu si (B) ne suffit pas (Q5-3 pour le côté réflexion). Compatible H-a. Tests `ResolverTest`/`PluginLoaderTest` + un cas « valeur hors table ».

**Q4-8** [G / haute / M] — `kClauseEvaluators` (`Condition.cpp:74-84`) + `conditionSummary` (`:118-151`, if-chain des mêmes 8) + `Keywords.cpp:33` (5 sur 8, Q2-42) + `ConditionBuilder.cpp:59`. *Action* : `constexpr array<ClauseKindInfo, 8> kClauseKinds { name, eval, summary, fields }` + `static_assert` ; les quatre consommateurs lisent la même table. `ConditionTest`.

**Q4-9** [G / haute / M — T-4] — `DamageType` : sept chaînes de traits dans cinq fichiers (`Damage.cpp:10, 49, 56, 35`, `EquipmentStats.cpp:9-21, 52-61, 77-79`, `CharacterStats.cpp:58-86`, `MeleeStrike.cpp:28-31`), 43 sites ; type du coup décidé 2× différemment (`maybeRollInjury` dominant vs `resolveStrikeDamage` premier canal). *Action* : `constexpr DamageTypeTraits kDamageTypes[12]` + `dominantDamageType()` ; cinq fonctions deviennent des boucles. `TypedDamageTest`, `EquipmentStatsTest`, `MeleeStrikeTest` ; visible seulement sur les armes multi-canaux.

**Q4-10** [G / haute / M — = Q2-16, une seule brique] — matrice des sorts écrite 3× : `Spells.cpp:91-161 spellSupported` (`if × switch`, 15 cellules), `LandscapeScene.cpp:5160-5362 executeSpell` (mêmes cellules → `PendingSpiritAction::Mode`), `SpellsTest.cpp:98-132`. *Action* : `constexpr SpellCell kSpellMatrix[] { element, verb, trajectory, channeled, Mode }` dans `world/spirit/` ; `spellSupported = findCell`, la scène lit `cell->mode`, le test parcourt la table (T-12). Nul ; `SpellsTest`.

**Q4-11** [G / moyenne / M] — `tickBuildup` (`StatusBuildup.cpp:70-113`) : trois DoT de même forme + trois comportements de déclenchement en `if` par type **dans** la boucle sur `kRows` ; « Add one line here per status ». *Action* : colonnes `OnTrigger`, `DotSource`, `dotRate`, `regenAttr`, `regenMult` sur `TypeRow` ; `BuildupTickResult` par colonne ; `applyBuildupResult` reste l'unique écrivain. Bit-exact ; `StatusBuildupTest`.

**Q4-12** [G / moyenne / M — décision T-11] — `Injuries.cpp:18-165` : sept switches de traits + trois `EffectForm` littéraux en C++ (`applyInjuryEffect:103-138`), idem `Survival.cpp:33-39`. *Action* : (a) `constexpr InjuryTraits kInjuries[3]` bit-exact, ou (b) `InjuryEffectForm` child-records (blessures moddables, STATS-CONFORMANCE #18) ; `makeInternalEffect()` dans les deux cas. `InjuriesTest`.

**Q4-13** [G / moyenne / S] — cinq compte-à-rebours « timer + tag » (`Damage.cpp:163-235`), déclencheurs timer+tag en deux endroits (`applyDamage:138-156`, `GameTime.cpp:60-71`), appels en ligne `CharacterTick.cpp:52-56` ; extension GAS directe pour 3/5 seulement et change le format de save (`CombatState.*Seconds` persistés par nom). *Action* : `constexpr TimedState kTimedStates[] { timer, tag, OnExpire }`, `grantTimedState()` seul écrivain, `tickTimedStates()` ; migration GAS = décision dev. Bit-exact.

**Q4-14** [G / moyenne / M] — le cœur GAS connaît le contenu : paires vital/max ×2 (`GameplayEffects.cpp:86, 150`), `attr("energy")` spécial (`:257`, `GameplayAbility.cpp:28`), `kEnergyRegenDelay` en dur, 125 `attr("…")` (35 registrations), 34 `"State.*"`, 25 `"Status.*"` sans constantes. *Action* : `constexpr VitalPair kVitals[3]` dans `Attributes.hpp` ; `namespace tags { constexpr string_view … }` à côté de `registerCharacterRuntimeTags` ; `kEnergyRegenDelay` → `StatsTuningForm` ou `[cpp-tuning]`. Nul.

**Q4-15** [G / moyenne / M] — items sans base commune : quatre sondes par type (`EquipmentStats.cpp:103-234`), `weight/goldValue/unremovable` déclarés 4× (`CoreForms.hpp`) ; `TypeInfo::isA` **existe** (10 sites). *Action* : `struct ItemForm : Form` parent des quatre, `findAs<ItemForm>` par `isA` ; save/TOML inchangés. `EquipmentStatsTest`.

**Q4-16** [G / moyenne / S — T-12] — quatre vocabulaires sans enum dans `gameplay/` : `AiPackageForm.kind` (commentaire `AiForms.hpp:28`, parsé dans `game/NpcScheduleController.cpp:109-136`, 3e copie `Keywords.cpp:43`), `FollowerClassForm.combatStyle` (« CombatAi vocabulary » inexistant, `== "healer"` ×2 dans `game/`), `FurnitureForm.category` (Q2-17), noms d'événements hashés par `eventKind()` sans registre (25 distincts, une faute de frappe TOML = règle muette). *Action* : `enum class PackageKind` + `kPackageKinds[]`, `enum class CombatStyle` dans `CombatSituation`, `EventBus::knownKinds()` + `cooker validate`. Nul ; `CuesSchedulesTest`, `CombatAiTest`.

**Q4-17** [G / moyenne / S] — `GroundClass` (`SpiritFire.hpp:37-40`, `kNames` `:38`) : cinq classes de sol en dur alors que `SpiritRuleTable.classes` les compile depuis la data (huit promises) ; ordre couplé implicitement à `SplatFamily`. *Action* : `fuelFromWeights(table, weightsByClass, wetness)` indexé par `classIndex`, poids construits depuis `kSplatLayers[].family` (Q3-21). Bit-exact si l'ordre des cinq est conservé ; `SpiritFireTest`.

**Q4-18** [G / moyenne / S] — `FieldFlags` (un bit `Transient`) : l'index enfant ne connaît que le nom littéral `"parent"` (`FormDatabase.cpp:31`) → trois index privés dans `buildQuestIndex` (`Quest.cpp:157-170`, parents `state`/`branch`) ; quatre champs morts gardés par prose (`ssaoStrength`, `skyCloudRays` avec date, `swimmable`, `castsShadow`). *Action* : `ParentLink = 1u << 1` → `indexForm` bucket tout Guid flaggé, `childrenOf<T>(db, parent, fieldId)` ; `Deprecated = 1u << 2` → loader loggue une fois, grille masque, `diffToRecord` n'émet pas, `Validate` compte. Aucun effet disque ; `FormIndexTest`.

**Q4-19** [G / basse / S] — petits vocabulaires et unions : `EditOp` taguée par « quel champ est rempli » (`EditSession.cpp:142-186`) → `EditOpKind` + switch ; `followerStance` `f32` décodé `== 1.0f/2.0f/3.0f` (`Followers.cpp:490-504`) → `u8` borné puis T-3 ; `followerCategory` deux if-chains (`:466-488`) → `PartyBucket` ; `toEmitterParams` ternaires + 3 bools (`GameplayCues.cpp:35-59`, avec Q5-7) ; `Perception.state` i32, `LightForm.kind/shadowMode` (`Spawner.cpp:29-34`), `MarkerForm.kind` « spawn/idle » sans consommateur ; `editorId` dupliqué : premier gagne (`findByEditorId`) vs dernier (`TextTable`), aucun lint → `ValidationReport.duplicateEditorIds` + une règle écrite. Nul.

**Q4-20** [G / moyenne / M — T-1] — les miroirs tenus par commentaire : `SavedStatsForm` 3e écriture de 55 champs de dix composants (`SaveForms.hpp:27-165`, un champ oublié n'est pas sauvé silencieusement) ; `LandscapeTuningForm` 4 « defaults MUST match » dont un seul testé (`TerrainGenTuningTest`) ; `kCoreAttributeNames`/`kCoreFields` « MUST match » (`Followers.cpp:226-252`) + `classAttributesAt` 9 champs à la main (`FollowerForms.cpp:19-32`). *Action* : test headless « tout champ réfléchi d'un composant persisté existe sur `SavedStatsForm` sauf exclusion » ; `kSavedComponents[]` ; table `{ name, member, physical }` ; à terme génération depuis `TypeInfo` (`FieldLane` T-1 — décision dev). `SaveStateTest`, `FollowerFormsTest`.

### D — duplication

**Q4-21** [D / haute / S — T-9, H-b ré-ouvert] — deux `Stager` identiques (`DungeonRecords.cpp:49-119`, `MapRecords.cpp:25-46`), `findField(field)->id` sans test (`:100`, `:39`), **14** déréférencements nus dans le dépôt (`SaveGame.cpp` ×9, `TerrainSculptTool.cpp` ×3, `SaveState.cpp:150`, les stagers) ; `Stager::live()` = 2e copie identique de `copyFields` (`EditSession.cpp:13-17`) — preuve nouvelle vs H-b (« un appelant chacun »). *Action* : `data::RecordStager` (`ensure`, `set` assertif, `stageAll`, `live` + callback d'index) ; `reflect::copyFields` public ; `copyMatchingFields` reste l'opération distincte. Ferme Q1-38. Nul.

**Q4-22** [D / moyenne / M] — trois A* sur un squelette (`Pathfinding.cpp:28-80`, `TerrainNavigator.cpp:56-170`, `InteriorNavigator.cpp:21-164`). *Action* : `ai::astar<Node>(start, isGoal, forEachNeighbor, heuristic, budget)` header-only dans `engine/nav/`, trois adaptateurs de ~40 l. ; honore le contrat (Q4-29). Bit-identique si ordre d'expansion et tie-breaking conservés ; `TerrainNavigatorTest`, `DungeonNavTest`, `MovementTest`.

**Q4-23** [D / moyenne / S] — triade onyx/amber/garnet en **sept sites** de `gameplay/` (`ResonanceDecays.cpp:11`, `CharacterTick.cpp:60`, `GameTime.cpp:95, 141`, `GameplayEffects.cpp:472`, `Injuries.cpp:109`, `MeleeStrike.cpp:48`, `Resonance.cpp:55-57`) ; pré-scan « decay on expiry » **verbatim** (`CharacterTick.cpp:58-72` / `GameTime.cpp:140-155`) ; `foldMul` ×5, « find puis has » ×8, « find puis add » ×5. *Action* : `kResonanceTriad[]` + `isResonanceAttribute()` dans `Resonance.hpp`, `collectExpiringDecays()`, `StatModifiers::multiply/add`, `hasTag/addTag` à côté de `syncStateTag`. Bit-exact ; `ResonanceTest`, `DrugsTest`.

**Q4-24** [D / basse / S] — six résolveurs de singleton de tuning (`LandscapeForms.cpp:44-83` ×5, `StatsTuning.cpp:55`) + 5 accesseurs de guid d'une ligne. *Action* : `template <class T> data::resolveSingleton(db, guid)` dans `FormQuery.hpp` + `kSingletonGuids`. Nul.

**Q4-25** [D / basse / S] — petits doublons : `enterState` ×2 (`Quest.cpp:84-113`) ; rayon du disque noyau `4.0f` ×4 (`Spells.hpp:55`, `SpiritSources.cpp:79`, `WaterReading.cpp:172`, `LandscapeScene.cpp:7571`) → exporté par `WaterSim` ; balistique ×3 → `BallisticArc` ; `riverAt` (`WaterReading.cpp:30-56`) = 11e copie du prédicat eau → `WaterBodies::riverAt` (T-7) ; π/τ ×7 → `core::kPi/kTau` (avec Q3-42) ; bracket `self`/`target` Lua ×5 (`Vm.cpp:233-456`) → `SelfScope` RAII ; `callBrain` 9 champs à la main → `tableFromReflected` ; `fire(node)` ×2 (`Dialogue.cpp`) ; `dispatchTriggerEvent` ×2 + `collectBoxes` ×2 (`TriggerSystem.cpp`, `Collision.cpp`) ; clé XZ ×13 dans le dépôt + `cellOf` ×4 → `core::packXZ` (Q3-39) ; scan manuel U8-1 survivant ×2 (`Factions.cpp:38`, `WorldModel.cpp:16`) → `data::forEach` ; matcher « événement + filterTag + partie » ×3 (`Followers.cpp:198, 513`, `Quest.cpp`) ; `rankOf` ×2 (`Resolver.cpp:32, 118`). Tous bit-identiques.

### B — bugs

Voir la table : Q4-26 (ids d'effets appariés, prouvé, latent), Q4-27 (`convertValue` tronque), Q4-28 (`baseDir` du cuit), Q4-29 (contrat `Navigator`), Q4-30 (`addItem` sans contrat).

### H — hygiène

**Q4-31** [H / basse / S] — commentaires hors politique ~60 sur 22 000 l. (data 10 dont la date `2026-07-30` `LandscapeForms.hpp:248` ; GAS 16 « Phase-3 » ×5, « chantier CARTES M4.4 », « audit U5-3 » ; actors 5 + 7 « HOW TO FILL » = plans de travail dans le code, à trancher ; world ~18 « chantier CARTES M2.2/M3.3 » ×9, « brick d/e », `Movement.hpp:13` périmé ; spirit 11 « Chantier ESPRITS » en en-tête ; « Subsystem map: docs/AUDIT/U6-U8 » pointent vers un audit clos → `PHASE-4`/`HORIZONTAL-PASS`) ; données mortes : `SpiritForm.triad/channel/fieldKind` **sans lecteur**, `bleedBurstDamage` « OBSOLETE » réfléchi, `armorRating` sans lecteur, `MarkerForm.kind` « spawn/idle », `WorldForms.hpp:36` cite `game::kMapTilesPerSide` depuis `world/`. *Action* : tri par fichier touché ; retirer ou lire les trois champs d'esprit (les lire = valider data ↔ enum au `compileSpiritRules`).

**Q4-32** [H / basse / S] — divers : constantes de formule sans `[cpp-tuning]` (`CharacterStats.cpp:94, 166, 173, 180, 183`, `EquipmentStats.cpp:134-157`, `Riding.cpp:28`, `Swimming.hpp:28` défauts = 2e source de vérité) ; `DungeonRecords` `editorId[96]` + `snprintf` ×12 (troncature), borne magique `64` du sweep des leviers (`:505`), `kPatrolMarker = 0x06` saute `0x05` ; sept `static_cast` de catégorie sans assert (`Spawner.cpp:45-101`) → `baseAs<T>` avec `isA` ; `SpiritJetList::tick` perd l'émetteur sans `stopped` (`SpiritJets.cpp:72-95`) ; `Rest.{hpp,cpp}` = 26 l. pour une ligne ; `SavedEffectForm.attribute` = fnv brut dans le TOML (assumé). Nul.

### P — optimisation évidente

Aucune. `EventBus::dispatch` (`find_if` par handler), `attr()`/`tags.find()` résolus à chaque coup (4 + 4 par frappe), `readWater` balayant 513², double parse glTF : notés sans mesure, absorbés par Q4-14 pour les constantes.

---

## Positifs (modèles à citer dans les corrections de Q1-Q3)

- **`data/`** : `Resolver::resolve` (180 l., trois phases, warnings qui disent QUI et QUOI),
  `diffToRecord` (la règle §5 en un lieu), `FormDatabase` (index maintenus à l'`add`, contrat
  « handle order = load order »), `CsvImport` (identité déterministe, mode patch = §5 pur),
  `Synthesis` (§5.1 sans mécanisme nouveau), `Validate` (lint par réflexion), `EditSession`
  (gestes d'undo/redo, drafts clone-on-write, export = plugin ordinaire),
  `PluginConfig::loadPluginStack` (absent ≠ erreur), `BinaryFormat` (**le** `BinaryArchive` de
  T-5 en tout sauf le nom), 20 fichiers de tests.
- **GAS/stats** : `kRows` + `static_assert` (la forme de table), `DerivedStatRegistry` +
  `StatView` (formules = données capturant le tuning), `applyBuildupResult` et
  `ageAndExpireEffects` (partages de juillet tenus), `payAbilityCost` (« the same effect,
  scaled : still the pipeline »), `TagContainer` ref-compté, `StatsTuningForm` (105 knobs,
  défauts = livrés), save « never fatal » sur tag inconnu, `applySavedState` ordonné.
- **Acteurs/combat** : `resolveMeleeStrike` + `StrikeContext` avec sinks optionnels ;
  `setSwingPhase` / `updateLifeState` / `setFollowerStance` (« toutes les transitions par une
  fonction ») ; `decidePlayerAction`, `chooseCombatMove`, `decideMoveMode`, `decideFollow`,
  `adoptOnHit` (inputs scalaires, sortie enum) ; `kClauseEvaluators` ; `CueTable::find` (repli
  hiérarchique) ; `EventBus::dispatch` (ré-entrance spécifiée) ; `evaluateSchedule` (« last in
  load order wins » = la superpuissance child-record) ; `Followers.hpp` « Reused systems
  (§2.11) » par section ; `grantPerk` dedup par `grantedTag` ; `segmentHitsCapsule` (la lame
  visible touche, jamais un cône pour le coup).
- **World** : `Spawner` (table catégorie → fonction, universels par `findField`, « le record
  matérialisé par un save prend le pas sur le template ») ; `AnimBridge::GraphForms` (UNE
  mapping, deux sources) ; `Perception` (`hearNoise` ré-entre par LA transition) ;
  `TriggerSystem` (snapshot-puis-agir, latch `once` réfléchi) ; `CellStreamer` (synchrone par
  décision) ; `WorldModel::materializeCell` idempotent ; `TerrainNavigator` (grille paresseuse,
  rien à baker) ; `InteriorNavigator` (« costs only grow, heuristic stays admissible ») ;
  `DungeonRecords` identités dérivées par famille ; `KillZ` (mort par le pipeline, jamais un
  téléport).
- **Esprits/script/quêtes** : `SpiritRules` (shifumi dérivé de l'enum, acteur non-esprit
  **rejeté par la forme de la table**) ; `compileSpell` (le nom du champ fautif dans l'erreur) ;
  `DialogueRunner` (97 l., réutilise tout) ; `Quest` (`onQuestEvent` pur, save par records) ;
  `Vm` (sandbox, `ScriptSelf` refuse l'écriture d'attribut, coroutines re-résolues par handle,
  `valueToLua` par `visit`) ; `SpiritSourceList`/`SpiritJetList` (capacité bornée, guids
  déterministes) ; `advanceTreeFire` ; `runFireJob` (Phase 5 au mot).

---

## Ordre de correction proposé (à arbitrer)

Un commit par brique ; la simulation a la couverture headless : `-tse=slow` Debug avant commit,
suite complète Release avant push.

1. **Bugs et plomberie sans risque** : Q4-26 + la moitié `ActiveEffect` de Q4-4 (un commit, un
   test), Q4-27, Q4-28, Q4-29 (contrat écrit), Q4-30, Q4-23 (helpers un par commit), Q4-24,
   Q4-25 (a-o).
2. **Les mécanismes `data/`** (ferment Q1-38 et T-9) : Q4-21 (`RecordStager`, `copyFields`
   public), Q4-18 (`ParentLink`, `Deprecated`), Q4-19 (`EditOpKind`, lint `editorId`) ; puis
   Q4-1 (`stageDungeonRecords` découpé sur le stager).
3. **La décision T-3** (Q4-7 avec Q5-3) : (B) `FieldMeta`/`EnumTable` d'abord ; dès qu'elle est
   actée, Q4-8 (`kClauseKinds`, ferme Q2-42), Q4-16, Q4-17, le reste de Q4-19, et les tables
   de noms de Q2 (`Keywords.cpp` disparaît).
4. **Les tables du GAS et du combat** (T-4, bit-exact) : Q4-9 (`kDamageTypes`), Q4-11
   (colonnes de `kRows`), Q4-14 (`kVitals`, `tags::`), Q4-13 (`kTimedStates`), Q4-15
   (`ItemForm`), Q4-10 (`kSpellMatrix`, **avec Q2-16**).
5. **La persistance** (T-13, T-1, T-5 — décisions dev) : Q4-2 (`attachActorKit`, ou le pont
   `reflectedComponent` de Q5-5), Q4-20 (test de couverture du save puis génération), Q4-3 +
   la moitié `ScriptVars` de Q4-4 (kind conteneur ou child-records), Q4-12 (table ou data),
   Q4-6.
6. **Nav** : Q4-22 (`astar<Node>`) qui honore Q4-29.
7. **Structure et hygiène** : Q4-5 (`Followers` en cinq en-têtes), Q4-31 par fichier touché,
   Q4-32.
