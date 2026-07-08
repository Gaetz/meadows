# U8 — Narratif + script + seams + cooker (audit)

> **✅ MISE À JOUR 2026-07-08 — U8-3 + U8-9 FAITS, précisions.** U8-3 (`3a02d8e`) :
> source unique d'enregistrement des Form-types. La description « 3 sites (cooker,
> EditorScene, AllForms) » était **inexacte** — EditorScene n'enregistre rien (il
> appelle `registerAllFormTypes`) ; il n'y avait que **2** sites complets. Réalisé
> **sans nouvelle lib** (la « aggregation lib » que l'auteur jugeait non rentable) :
> le cooker compile directement `game/AllForms.cpp`. U8-9 (H-a, `f2865bd`) :
> `valueToLua` passé au `visit` exhaustif (`luaToValue` reste un switch —
> construit depuis un type Lua externe). Voir `README.md` §0/§H-a.

Périmètre : `quest/` (Quest, Dialogue), `script/` (Vm, ScriptVars),
`engine/physics/`, `engine/audio/`, `engine/ui/`, `engine/ecs/`,
`tools/cooker/`. Rubric = plan d'audit `j-ai-fait-une-purrfect-rocket.md`.

## Hard-checks demandés (pass/fail explicite)

- **§2.8 (script/) — PASS.** État persistant par entité = composant réfléchi
  `ScriptVars` (`std::unordered_map<str, reflect::Value>`, ScriptVars.hpp:18),
  PAS des tables Lua libres. VM unique, partagée et sandboxée (sol2 derrière
  pimpl, Vm.cpp:157-196) ; `self` = proxy sur les composants de l'entité
  (ScriptSelf, Vm.cpp:63-153) ; attributs lisibles seulement, écriture rejetée
  (Vm.cpp:99-103, §2.9) ; RNG déterministe via `core::Rng` (Vm.cpp:167,322).
  Latence via coroutines + scheduler central (`std::list<Coro>` +
  `tickCoroutines`, Vm.cpp:265-312), pas d'env Lua par entité. Conforme.
- **Fuite de façade (headers) — PASS pour les 3 seams.** Aucun type tiers ne
  traverse `Physics.hpp` / `Audio.hpp` / `UiSystem.hpp` (les occurrences
  "Jolt"/"ma_"/"Rml" y sont uniquement en commentaires). Types opaques :
  `BodyId=u64`, `SoundParams`, `UiRow`. Libs liées **PRIVATE** à leur dép tierce
  (engine/CMakeLists.txt:83 Jolt, :94 RmlUi, :105 miniaudio ; script/CMakeLists
  sol2 PRIVATE). Pimpl (`uptr<Impl>`) partout.
- **flecs confiné — FAIL partiel (inter-unités).** `#include <flecs.h>` +
  `flecs::entity` dans deux headers **gameplay** : `gameplay/save/SaveState.hpp:3`
  et `gameplay/actors/CharacterTick.hpp`. `engine/ecs/World.hpp` expose flecs
  volontairement (§3, non-façade assumée) ; la dérive est côté U6 (voir F2).
- **H-c (dispatch parallèle) — CONFIRMÉ (inter-unités).** `UiSystem`
  possède son propre canal d'événements `UiModelEventHandler`
  (UiSystem.hpp:47, dispatché UiSystem.cpp:664) — 3e mécanisme de dispatch avec
  EventBus + CueRegistry. Nuance : ici c'est un **callback unique** (remplacé
  par `setModelEventHandler`), pas un bus multi-abonnés. Voir F8.
- **cooker réutilise le chemin résolveur/binaire — PASS.** Main.cpp appelle
  `loadPluginFile` / `writePluginBinary` / `readPluginBinary` / `writePluginToml`
  (Main.cpp:125-158) — aucune ré-implémentation de BinaryFormat/TOML. Bon.
- **ImGui hors RHI (concern U2 dans engine/ui) — CONFIRMÉ.** `ImGuiLayer.cpp`
  utilise le backend GL propre d'ImGui (`imgui_impl_opengl3`, lignes 4,31,53),
  hors `rhi::` (§2.1). À l'inverse **UiSystem (RmlUi) est RHI-propre** (aucun
  appel GL ; rendu via `rhi::CommandBuffer`). Voir F5.

## Findings

| id | sév | axe | fichier:ligne | description | action | effort | inter-unités |
|----|-----|-----|---------------|-------------|--------|--------|--------------|
| U8-1 | med | réutil | quest/Quest.cpp:15-26 ; quest/Dialogue.cpp:14-25 | `forEachForm<T>` réimplémenté à l'identique dans les deux fichiers ; duplique `data::forEach<T>` (FormQuery.hpp:23). Quest.cpp utilise déjà `data::forEach` dans applySavedQuests → incohérence interne. | Supprimer les deux templates locaux, appeler `data::forEach`. | S | oui (data/) |
| U8-2 | med | archi | gameplay/save/SaveState.hpp:3 ; gameplay/actors/CharacterTick.hpp | `flecs.h` + `flecs::entity` dans des headers gameplay, hors `engine/ecs` alors que §3 vise flecs « confiné à meadows-ecs ». | Passer par une abstraction ecs ou déplacer ces signatures ; à trancher côté U6. | M | oui (U6) |
| U8-3 | high | factor | tools/cooker/Main.cpp:189-205 | Liste de `registerXxxFormTypes` recopiée à la main (le commentaire admet « Keep in sync with EditorScene::onEnter » et signale un bug passé : ne cookait que CoreForms → records droppés). 3 sites de vérité (cooker, EditorScene, AllForms). | ✅ FAIT (`3a02d8e`). Précision : seulement **2** sites réels (EditorScene appelle `registerAllFormTypes`, il n'enregistre rien). Le cooker compile `game/AllForms.cpp` — **sans nouvelle lib**. Voir bandeau. | M | oui (U5/U7) |
| U8-4 | med | qualité | script/Vm.cpp:161-163, 265-283 | Les `Coro` conservent des `ScriptContext` avec pointeurs bruts vers les composants pour toute la durée du wait ; aucun cancel-on-death ni détection de handle mort → dangling si l'entité meurt / changement structurel ECS pendant l'attente. Risque documenté dans le header mais non appliqué. | Indexer les coroutines par `Entity` et purger à la mort / avant mutation structurelle. | M | non |
| U8-5 | low | archi | engine/ui/ImGuiLayer.cpp:4,31,53 | Backend GL propre d'ImGui, hors RHI (§2.1). Exception dev-tool assumée (§3 « Dev tools stay ImGui ») mais chemin GL réel hors backend. | Documenter comme exception tolérée ; ne pas étendre. UiSystem/RmlUi reste la référence RHI-propre. | S | oui (U2/U3) |
| U8-6 | low | qualité | quest/Quest.cpp:48-99 ; quest/Dialogue.cpp:65-93 | `onQuestEvent` imbrique branches×tâches (+ `branchComplete` re-scanne les tâches), chaque `forEachForm` étant un balayage O(total forms) de la DB ; `Dialogue.options/select` scannent toute la DB par appel. OK au N proto, à surveiller. | Indexer par parent (child→parent Phase-2) si la DB grossit. | M | non |
| U8-7 | low | qualité | script/Vm.cpp:252 | Handler d'événements Lua avale les erreurs (`(void)fn(payload); // errors swallowed for 4b`) → échecs de script silencieux. | Logger l'erreur (LOG_WARN) au minimum. | S | non |
| U8-8 | low | factor | engine/ui/UiSystem.hpp:47 ; UiSystem.cpp:656-665 | `UiModelEventHandler` = 3e canal de dispatch ad-hoc (H-c). Callback unique, pas un bus. | Candidat au `Signal<Payload>` mutualisé de core (Tier-3). | M | oui (H-c) |
| U8-9 | low | réutil | script/Vm.cpp:26-59 | `valueToLua`/`luaToValue` = encore une chaîne `if constexpr` par kind de `reflect::Value` (famille H-a du switch FieldKind). Petit mais récurrent. | Rattacher au visiteur `reflect::Value` unique si créé (Tier-3 H-a). | S | oui (H-a) |
| U8-10 | low | propreté | tools/cooker/Main.cpp:172-181 | `new-guid` : `count` via `atoi` non borné (négatif → boucle vide, silencieux). Cosmétique. | Clamp `count >= 1`, message si arg invalide. | S | non |

## Verdict
`script/` et les seams (physics/audio/ui headers, ecs) sont **sains** :
§2.8 respecté au mot près, façades étanches, deps tierces PRIVATE. Le cooker
réutilise correctement le pipeline binaire. Points chauds : la duplication de la
**liste d'enregistrement des Form types** (U8-3, déjà cause d'un bug avéré) et le
**dangling potentiel des coroutines** (U8-4). Duplications transverses mineures
(forEachForm, conversion reflect::Value, dispatch UI) à remonter au Tier-3.
