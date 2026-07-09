# Audit Meadows — Synthèse

> Point d'entrée unique de l'audit multi-unités (U1–U9 + deep-dive §2.9).
> Rapports détaillés par unité dans ce même dossier. **Aucun code modifié** :
> ce document agrège et priorise ; le dev arbitre.

---

## 0. Avancement de la remédiation (2026-07-07)

Bricks livrées (Batch 1 « gains rapides », cadence brique-par-brique, chacune
buildée + 279 tests verts) :

| Finding | Fix | Commit |
|---------|-----|--------|
| U1-09 / U2-09 | Suppression de l'arbre mort `_old/renderer_test/` (164 Mo) | `c9d97ca` |
| U6-F8 | Retrait du footgun public `setCurrentValue` | `0b86235` |
| U1-01 / U7-3 | `static_assert` verrouillant l'ordinal `FieldKind`↔`Value`↔`KindOf` | `25b0480` |
| U7-1 | `HeightPatch`/`HeightPatches` sortis de `render/` → `engine/terrain/` (§2.10) | `16c7079` |
| U1-05 / U3-2 | `hashU32`/`HashRng` hoistés dans `core/Hash.hpp` (7 fichiers dédupliqués) | `9fe48a6` |
| U8-2 | Fuite `flecs` hors des headers `gameplay/` (→ `ecs::Entity`) | `20f3e12` |
| U2-05 / U6-F9 / U5-7 / U2-08 | Label d'erreur shader + commentaires périmés | `1b88290` |
| U6-F1 §2.9 | **Option A** : « execution calculations » sanctionnées dans CLAUDE.md §2.9 + marqueurs sur les sites (aucun changement de comportement) | `a51fae7` |
| STATS (bug) | DoT ignition/électrocution réduits par **vitality** (était `will` — bug copier-coller vs STATS.md §183) | `f98b6dc` |
| STATS (bug) | **Bleed** = criticalSensitivity% de maxHealth, **ignore l'armure** (était un forfait slash mitigé par l'armure) | `4d6a71a` |
| STATS (tests) | Non-régression : DoT ignition/électro ∝ vitality (pas will) ; bleed = crit-execution sans armure | `95a0f14` |
| U8-3 | **Source unique** d'enregistrement des Form-types : le cooker compile `game/AllForms.cpp` (`registerAllFormTypes`) au lieu de recopier la liste — plus de dérive possible ; sans nouvelle lib ni changement de DAG | `3a02d8e` |
| H-a (1/3) | `engine/reflect/Visit.hpp` (`overloaded`+`visit`) : dispatch `Value` **exhaustif** (un kind oublié = erreur de compil, plus de corruption silencieuse). Appliqué au binaire + TOML (octets identiques, MD5 inchangé) | `4ac53a5` |
| H-a (2/3) | `visit` sur les sites éditeur/script (valueRepr, PropertyGrid, Vm). Constat : valueRepr ≠ valueToString (pas le doublon supposé par U5-1 — formats distincts) | `f2865bd` |
| H-b | `data::diffToRecord` : la règle §5 « record = seulement ce qui change » en **un seul endroit** (EditSession + SaveState). `copyFields`/`copyMatchingFields` laissés en place (opérations distinctes, 1 appelant chacune — déplacer n'enlèverait rien, §10) | `40c9fcf` |

**Reporté (décision dev requise, PAS un oubli) :**
- **U6-F1 §2.9 — RÉSOLU (Option A, `a51fae7`)** avec le dev : les écritures de tick
  (DoT, regen, mort par buildup) sont des « execution calculations » sanctionnées
  (CLAUDE.md §2.9 amendé). La vérification a confirmé que l'Option B (router le DoT
  via `applyDamage`) aurait été une **régression** (double-mitigation) — la résistance
  agit sur le seuil de buildup, pas sur le dégât par tick — et a révélé en prime deux
  bugs d'équilibrage, corrigés (`f98b6dc`, `4d6a71a`, cf. table §0).
- **Batch 2 (H-a/H-b) — FAIT** (`4ac53a5`, `f2865bd`, `40c9fcf`, en session
  supervisée). Constat transverse : les findings H-a/H-b (comme U5-1) **sur-groupaient**
  des choses distinctes — la « table monolithique » H-a violerait §2.10 (couplerait
  binaire↔ImGui↔Lua), valueRepr n'est pas valueToString, et les deux clones ne sont
  pas un doublon. Le vrai gain livré : exhaustivité compilée du dispatch `Value`
  (filet sur le seam §5) + la règle §5 diff en un seul endroit.
- **Batch 3 (structurel : décomposition LandscapeScene, seam Phase-5)** — **EN COURS**
  (session supervisée, cadence brique-par-brique ; LandscapeScene n'a pas de couverture
  headless → build OK → validation en jeu par le dev → commit). **État au 2026-07-08 :**
  - **Fait & commité :**
    - Architecture des 3 modes de jeu (Play/Spectator/Edit) : enum `SceneMode`,
      `simPaused`, hotkeys F2/F3, Escape→`lastActiveMode` (`b97d2b5`, `fa40956`).
    - **U4-5 — `SceneEditor` + `TerrainSculptTool`** extraits derrière `EditorContext`/
      `SculptContext` (`f20a7aa`, `9869fe6`) ; caméra éditeur (`FlyCamera::LookTrigger`),
      sculpt local+live (remeshChunks/invalidateChunks).
    - **U4-8 — `WeatherController`** (météo capture/apply/blend) — commité en session
      antérieure.
    - **U4-10 (partiel) — `StreamingController`** (ring cellules, snap, colliders, nav ;
      contrat `StreamingContext`) `124ae87` ; **`NpcDirector`** (sous-système NPC complet :
      structs Npc/RigData, liste, build/IA/schedule/combat/draw ; contrat `NpcContext`,
      délégateurs fins ; la scène lit via `npcDirector.npcs()`) `533edc9`.
    - **U4-10 (suite) — `InteractionController`** (`c5ea579`, validé en jeu
      2026-07-08) : prompt [E] (scan de visée + dispatch porte/objet/PNJ/
      cadavre/mobilier), machine de fade travel/rest (hold plancher inclus),
      toast (`say()`), `rest()`/`wait()` — derrière `InteractionContext` (refs
      + 4 closures : travel/openDialogue/openContainer/tryShowScreen).
      **`performTravel` RESTE dans la scène** (swap de worldspace = territoire
      streaming) ; la machine l'appelle via le callback `travel` au noir du
      fade. La scène lit via accesseurs (`promptVisible/talkVisible/fadeAlpha/
      fading/…`).
  - **Bugs open-world corrigés au passage** (pré-existants, PAS des régressions — chemin
    save intact vs la décompo ; prouvés par diff + tests) : régénération ressuscitant les
    morts (`isDead` non branché dans `tickGameTime`) `6e5b198` ; position runtime non
    réappliquée au reload de cellule (`PendingSaveLayer::applyReferenceOverrides`) `5a1787f`.
    Nouveau test `tests/DeathPersistenceTest.cpp` + cas position dans `CellDeltaTest.cpp`.
    - **U4-9 — `GameHud`** (`7732bc3`, validé en jeu 2026-07-09) : les 6
      méthodes de présentation (`updateHudModel`, `updateNameplates`,
      `pushItemModels`, `pushJournalModel`, `pushDialogueModel`,
      `updateMenuClockLine`) + l'état de vue (les 2 `InventoryView`,
      `dialogueOptions`) extraits derrière `HudContext`. Les ACTIONS de jeu
      restent dans la scène et mutent les vues via `hud.inventory()`/
      `hud.loot()`/`hud.dialogueOptions()` (le pattern `npcDirector.npcs()`).
    - **Fix hors-audit** (`c4d0eec`, validé en jeu) : focus clavier ImGui
      verrouillé après un aller-retour F3 (NavEnableKeyboard +
      WantCaptureKeyboard latché, souris capturée) — `enterPlayMode` et la
      fermeture de la console F8 relâchent le focus nav.
  - **NUIT DU 2026-07-09 (session autonome, dev absent — règle : uniquement
    du prouvable headless, chaque brique = build + suite verte + commit).**
    11 briques livrées, `2be3a10..cd29a87`, suite passée de 284 à 299 cas :
    - **U9-2** golden tests du contrat on-disk (ordinaux FieldKind 0..10 +
      layout octet-à-octet d'un plugin cooké) `2be3a10`.
    - **U6-F10** les recomputes partiels préservent les maxima dérivés
      (cache `derivedTargetIds` par acteur ; plus d'écrasement à 100 entre
      applyEffect et le recompute complet) `9d6f9ad`.
    - **U6-F2 + U6-F7** `applyBuildupResult` partagé tick réel/game-time —
      3 vraies divergences réconciliées : ⚠️ la mort par buildup temps réel
      ne zérotait PAS la santé (tag seul → résurrection au prochain sync +
      reload vivant, le jumeau du bug regen-revive) ; ⚠️ l'électrocution ne
      staggerait pas en game-time ; le tag State.Dead atterrit au tick du
      kill (refresh du current avant updateLifeState) `5e8a971`.
      **À valider en jeu : mort par poison/buildup, stagger électro.**
    - **U5-5** le chemin patch capture `scale` (perdu avant) ; le gate
      acteurs-seulement de position/rotation est DÉLIBÉRÉ (Y snappé) et
      désormais documenté `3d2e78a`.
    - **U8-4 + U8-7** Vm Lua : les coroutines portent un handle d'entité et
      re-résolvent les pointeurs composants à chaque resume (mort = abandon,
      archetype move = re-fetch — c'était de l'UB) ; les erreurs Lua sont
      loggées au lieu d'être avalées `76d7ca2`.
    - **U9-3** ✦ `meadows-render` extraite de la lib de base (render/ +
      rhi/ + ImGuiLayer + GlContext ; mêmes sources, mêmes flags) + cible
      **`meadows-simlink`** : link WHOLE_ARCHIVE de tout le sim SANS
      meadows-render — preuve négative vérifiée (une référence render
      injectée dans Combat.cpp casse le link, puis retirée). Résidu
      documenté : SDL/platform restent dans la base (§2.10-strict =
      décision à venir). Le cooker linke meadows-render (terrain-pad)
      `2e6a882`. **Dev : lancer le jeu une fois pour confirmer ; CLion
      reconfigurera seul.**
    - **U7-6** dépendances déclarées validées par le resolver (manquante ou
      chargée après → warning + compteur) ; **U1-02** collision de type-id
      réflexion assertée en debug ; **U8-10** `new-guid` borné ; **U9-5**
      test de la chaîne horloge→effets game-time ; **U9-1** tests
      d'`extractMeshes`/`collectLights` (les nourrisseurs headless du
      snapshot que U4-2 va consommer) `4ec8635`.
    - **U3-3** (moitié garde) `static_assert` sur chaque offset std140 de
      `FrameUniforms` + taille totale (le miroir GLSL n'était tenu que par
      commentaire) ; **U6-F3** les 2 boucles de tick d'effets partagent
      `ageAndExpireEffects` ; **U8-1** les clones de `data::forEach` dans
      quest/dialogue supprimés `7b4c7dc`.
    - **U1-04** `core::Clock` (steady-clock partagé : FrameProbe, dt moteur,
      budget terrain, RmlUi) `8bd080c` ; **U5-3** agrégateur
      `registerCharacterRuntimeTags` (les 2 scènes 2D avaient DÉJÀ dérivé :
      Shaken/CriticalWeakness/Exhausted inégaux) `c3e2972` ; **U1-08**
      `MeshData.hpp` relocalisé sous engine/assets (précédent U7-1)
      `cd29a87`.
    - **U4-1 (suite) — `PlayerController`** (`67d00ea`, validé en jeu
      2026-07-09) : la capsule cinématique + l'état de mouvement (vitesse
      lissée, cooldown d'attaque, accumulateur SprintCost, jumpSpeed
      fallback) et `update`/`tryAttack` (mouselook, déplacement
      caméra-relatif, saut, coût sprint §2.9, mêlée LMB + crime D2) extraits
      derrière `PlayerContext` (refs + `overencumbered` + le closure
      `syncWantedTag`). **Les transitions de MODE restent dans la scène**
      (enter/exit/restoreMode = plomberie SceneMode, cohérent avec la vision
      « éditeur = couche SceneStack ») et pilotent la capsule via
      `spawnBody`/`destroyBody` ; boot/travel/tp pareil ; les sites focus
      lisent `playerController.body()`. Scène : 4162 → 3958 l.
    - **U4-1 (suite) — `UiRouter`** (`622dfc8`, validé en jeu 2026-07-09) : le
      dispatch des data-events RmlUi (`handleUiEvent`), les actions de menu
      (`handleMenuAction` : save horodaté, liste de chargement, wait,
      play/mainmenu/quit), l'ouverture des écrans objets (inventaire/
      conteneur/marchand avec profil D1 + restock 24 h) et les actions
      gameplay (équiper/consommer/transférer/troc) extraits derrière
      `UiRouterContext` (refs + 9 closures : pushes de modèles, save/load,
      wait, flips de mode, quit). Le routeur POSSÈDE l'état partagé des
      écrans (containerEntity, barterMode, multiplicateurs vendeur) —
      GameHud le lit via accesseurs pour les prix. L'OUVERTURE du dialogue
      reste dans la scène (territoire quêtes). Scène : 3958 → 3635 l.
    - **U4-1 (suite) — `SaveController`** (`7e680aa`, validé en jeu
      2026-07-09) : la sérialisation disque
      (`performSave` : capture de tout le vivant + flush de la couche
      pending dans UN plugin ordinaire §5), les drapeaux de reload
      (`requestLoad`/`takeReloadRequest`) et la résolution du fichier de
      save que la scène ré-entre en dernière couche (`beginLoad`) extraits
      derrière `SaveContext` (refs + snapshot du WorldState : horloge/
      worldspace/caméra/mode/météo, + 2 closures : balayage des références
      vivantes, toast). **Le contrôleur POSSÈDE la couche pending**
      (PendingSaveLayer + `pendingLoadSlot`/`reloadRequested`/
      `loadedFromSave`) — exposée via `pending()` pour le streaming
      (captureCell/veto) et le spawn d'acteur (applyReferenceOverrides/
      actorState), les sites d'appel inchangés (pattern `hud.inventory()`).
      **La moitié load-APPLICATION** (WorldStateForm → restore horloge/
      worldspace/caméra) **reste dans `onEnter`** (tissée au cycle de vie de
      la scène : streaming, résolution worldspace, heuristique de spawn
      caméra). `finalizeActorSpawn` reste (logique de spawn, appelle
      `pending()`). Scène : 3635 → 3605 l.
    - **U4-1 (suite) — `QuestDirector`** (`4afe23b`, validé en jeu
      2026-07-09) : la machine de la quête démo
      (EasternMenace), son miroir dans les **tags joueur** (les conditions
      de dialogue ne lisent pas les composants), le miroir crime
      bounty→`Crime.Wanted`, et le **dialogue** (runner + ouverture) extraits
      derrière `QuestContext`. Le directeur possède `questLog`, le pointeur
      quête-démo, le `DialogueRunner` et le `dialoguePartner` ; la scène
      atteint les bouts partagés (log pour save/HUD/console, runner pour le
      HUD, partner pour le troc) via accesseurs. **L'`eventBus` reste un hub
      SCÈNE** (le dialogue ET le combat y publient) : les souscriptions
      restent dans `onEnter` (le `this` scène est stable pour la vie du bus)
      et **délèguent** au directeur (`acceptDemoQuest`/`handleQuestEvent`/
      `payFine`) avec un contexte frais. **`makeEvalContext` reste dans la
      scène** (contexte de condition joueur générique, nourrit aussi le HUD —
      évite un couplage circulaire Hud↔Quest). Scène : 3605 → 3500 l.
    - **U4-1 (suite) — `SceneConsole`** (`d5049a7`, validé en jeu
      2026-07-09) : l'**infrastructure** de la console
      dev F8 (l'`EditSession` de réflexion, la VM Lua, le `ConsolePanel`, la
      visibilité + le cycle de vie create/reset/toggle/draw) et le **god
      mode** extraits — 5 membres épars → un `SceneConsole`. Le combat lit
      `sceneConsole.godMode()`, F8 fait `toggle(playMode)` (relâche le latch
      de focus nav en fermeture Play, comme enterPlayMode). **Les corps de
      commandes** (spawn/tp/tgm/save/load/startquest/queststate/settime)
      **restent dans `createConsole`** : ils touchent les internes de la
      scène (spawn dans le monde, tp joueur, horloge), même logique que les
      souscriptions d'event bus — un contexte de 12 refs/closures serait un
      déplacement de couplage, pas une réduction. Réduction volontairement
      modeste (3500 → 3487 l.) : le gain est l'*appartenance* de
      l'infrastructure, pas le compte de lignes. **+ correctif UX** (signalé
      dev) : à l'ouverture, la console **prend le focus clavier**
      (`ConsolePanel::focusInput`) et, en Play, **libère la souris** +
      **gèle joueur/caméra** (WASD tape au lieu de marcher, plus de
      mouselook) ; fermeture → reprise de la capture (sauf modal ouvert) +
      relâche du latch focus. Build vert + 299 tests verts. À valider en
      jeu : ouvrir la console en Play et **taper directement sans cliquer**
      (souris libérée, caméra figée), chaque commande (spawn/tp/tgm/save/
      load/startquest/queststate/settime), Lua libre, `get`/`set` réflexion,
      tgm actif en combat (invulnérabilité), Escape après fermeture en Play.
  - **Le final U4-2 + U4-4 + U4-6 — FAIT en 5 briques (2026-07-09, session
    autonome à la demande du dev) — build vert + 309 tests verts à chaque
    brique, un commit/brique révocable individuellement. ✅ VALIDÉ EN JEU
    par le dev (2026-07-09 : « pas de bug graphique ») :**
    - **R1 — `composeFrameUniforms` (U4-6a)** `b627161` : l'assemblage
      FrameUniforms (~110 l. : base + override intérieur + grade +
      submersion + terrain-light + grass-bend + storm/rain + auto-expo)
      devient une fonction PURE dans meadows-runtime (`game/FrameComposer`),
      `ComposedFrame{base,resolved}` — `base` préserve le contrat du pass
      reflection (composition brute, non patchée). 7 doctests headless
      (l'intérieur zéroe le soleil, toggles A/B neutres, slots .w).
    - **R2 — le snapshot étendu (U4-2a, le finding #1)** `e2c60ff` :
      `RenderSnapshot` gagne `lights`/`shadowLights`/`shafts`/
      `waterVolumes` ; `MeshInstance` embarque tint/emissive/albedo résolus
      à l'extract (plus de `forms.find` au draw). Extracteurs headless + 3
      doctests. Plus AUCUN `world.handle()` dans le chemin de rendu hors
      NPC. Nuance : la SÉLECTION des 16 lumières utilise la caméra
      pré-update de la même frame (< 10 cm en Play) — imperceptible.
    - **R3 — NPC skinnés via le snapshot (U4-2b)** `589c290` :
      `NpcDirector::extract` remplit `snapshot.skinned` (pose COPIÉE +
      transform + tint + handles de géométrie résolus — le précédent
      sprite/TextureHandle) après l'update NPC ; l'état de draw par entité
      (palette SSBO, model UBO, groups) sort de `Npc` vers des slots
      mark/sweep côté renderer. NpcContext perd ses 4 champs GPU. Le seam
      Phase-5 est COMPLET. Un NPC saute son ombre sur sa 1re frame
      (avant : mesh dégénéré invisible) — équivalent visuel.
    - **R5 (avant R4, inversion délibérée : déplacer d'abord, RAII-ifier
      ensuite = chaque étape vérifiable) — `LandscapeRenderer`
      (U4-2c/U4-6)** `e3def86` : la classe qui possède la ShaderLibrary,
      les 14 systèmes `render::*`, tout l'état GPU, le frame graph complet
      et les panneaux dev terrain/rendu avec leurs toggles `*Ui` (la
      décision « les panneaux suivent leur état ») ; consomme UNIQUEMENT
      frame + snapshot + `RenderView` (caméra, atmos, interior, scalaires
      tuning, caches, gameUi, probe). Le sim atteint la vérité terrain via
      `terrainParams()` (collision/nav/snaps/sculpt/spawn), l'horloge ciel
      via `skySystem()`, le sculpt via les accesseurs de queues ;
      `applyTuning` seed les knobs depuis le TOML (§5). Création des
      systèmes regroupée dans `create()` (avant : éparpillée dans onEnter —
      indépendant de l'ordre). Scène : 3549 → 1848 l. ; son `render()` =
      construire la vue, tendre le paquet.
    - **R4 — handles GPU RAII (U4-4)** `059c953` : `rhi::Unique<Handle>`
      (`engine/rhi/UniqueHandle`, move-only, libère via son device à la
      destruction/reset/réaffectation, conversion implicite = sites de
      lecture intacts). Les ~40 handles autonomes ET les structs par entrée
      (MeshDraw/SkinnedDraw/LightShaft/WaterQuad) convertis : les gardes
      destroy-avant-rebuild disparaissent, les frees de mark/sweep
      deviennent `erase()`, le miroir manuel de destroy() (125 l.) devient
      des resets — un oubli n'est plus une fuite. `boundTexture` reste brut
      (vue non-possédante du cache). **U3-7 (frees PostFx) peut réutiliser
      le wrapper.**
  - **U3-1 (+ U3-4) — `ChunkStreamer` — FAIT `792bc57` (2026-07-09) —
    ✅ validé en jeu par le dev (2026-07-09) :** le ring implémenté 3×
    quasi-identiquement (queue
    worker estampillée génération, uploads budgétés compte + cap temps
    optionnel, requêtes centre-d'abord, éviction hystérésis) devient UN
    template `ChunkStreamer<Chunk, Payload>`
    (`engine/render/landscape/ChunkStreamer.hpp`) ; chaque système ne
    garde que ses différences en lambdas : le worker scatter/mesh,
    l'upload GPU (instances herbe / packing par variante veg / swap LOD
    terrain), les règles want/evict (les requêtes de changement de LOD et
    les compteurs pending du terrain inclus). U3-4 plié dedans :
    `chunkKey`/`chunkKeyCx`/`chunkKeyCz`/`chunkCoordOf` remplacent la
    danse pack/unpack u64 recopiée à la main dans les trois systèmes.
    −175 l. nettes ; **7 doctests headless** verrouillent la mécanique du
    ring (ordre centre-d'abord, budgets, slot libéré sur rejet, drop des
    générations périmées, hystérésis, aller-retour requête→worker→pump).
    Suite : 316 tests verts.
  - **Batch 3 : CODE TERMINÉ.** (Panneaux dev : `drawUi`/`drawSkyUi`/
    `drawGameplayUi` RESTENT scène par décision — hotkeys/overlays =
    contrôle de scène, ciel/météo = état scène (`atmos`/clock/weather),
    gameplay-debug = sim ; les panneaux terrain + rendu ont suivi leur
    état dans le renderer.)
  - **Lot « quick wins renderer » — FAIT 2026-07-09 (4 briques, build +
    316 tests verts chacune) — ⚠️ à valider en jeu (une session normale
    suffit : extérieur, hot-reload d'un shader, resize, quit propre) :**
    - **U3-5** `f030626` : `engine/render/MeshVertexLayout.hpp` — le
      layout MeshVertex écrit UNE fois (terrain lit+caster, veg lit,
      meshes de scène lit+caster) ; le caster végétation garde son layout
      position+uv (sway) — pas un doublon, laissé tel quel.
    - **U2-03/U2-06** `668254b` : `GlConvert.hpp` déclare les
      convertisseurs rhi→GL une fois (les 2 backends les re-déclaraient à
      la main) ; `GlDeviceBase::makePipelineState` porte la moitié
      raster-state de createPipeline (8 champs identiques 4.1/4.6).
    - **U3-7** `2ff24d8` : PostFx + Terrain/Grass/Vegetation passent à
      `rhi::Unique` — `PostFx::destroy` = le move-assign reset (~50 l. de
      destroy+null appariés supprimées), les buffers de chunks se
      libèrent à l'erase (les lambdas invalidateAll/evict ne gardent que
      leurs compteurs), swaps LOD et rebuilds de pipelines libèrent par
      affectation.
    - **U3-6** `045b091` : `kPassShaders` = LA liste des passes (la somme
      de générations était écrite 2×) ; les 4 cibles half-res + leurs
      groupes depth passent par un couple de helpers.
  - ✅ Briques de nuit validées en jeu par le dev (2026-07-09) : mort par
    buildup (poison à 0 → mort persistante au reload) et stagger
    d'électrocution confirmés.
  - **À VOIR AVEC LE DEV (reste, 2026-07-09) :**
    - *Perf : mesurer AVANT d'agir* (FrameProbe en jeu) : U2-01 (`::at` hot
      path draw), U7-7/U8-6 (scans O(n) FormQuery/quêtes).
    - *Arbitrage design* : H-c `Signal<T>` (EventBus/CueRegistry/UiModel),
      H-d `Handle<Tag>` (7 handles RHI + nextId — touche l'API RHI publique),
      U1-03 `core::Result` (introduire avec un premier chemin d'appel réel),
      U1-06 (contrat de durée de vie JobCounter), U2-04 (ShaderDesc/GLSL,
      dette Vulkan).
    - *Scène/éditeur (avec U4-1 suite)* : U4-7 (constantes → tuning), U4-11
      (strings FR → localisation : décision d'approche), U4-12/13/14
      (cleanups scène), U5-4 (3 éditeurs dev), U5-6 (gActive), U5-8/9/10,
      U9-7 (Value↔widget ImGui).
    - *Features absentes reclassées* (chantiers MEADOWS-PLAN, pas des trous
      de test) : U9-6 (alias de quêtes jamais implémentés), U9-8 (pas d'API
      begin/end use mobilier — seule l'occupancy existe).
    - *Pureté §2.10 stricte* : sortir SDL/platform de la lib de base
      (split meadows-core/meadows-platform) — étape suivante du verrou U9-3.
    - *U9-4* : mutualiser les helpers de test (~30 TUs) — churn, à faire
      opportunistement.
  - **Hors-audit, planifié post-audit :** chantier « cellules extérieures implicites »
    (`docs/IMPLICIT-CELLS.md`, lié dans MEADOWS-PLAN §chantier 2, commit `f0f7c00`) —
    n'impacte aucun finding, à faire APRÈS l'audit.
  - **Comment reprendre :** lire ce bloc + le finding visé dans la table §0 + les mémoires
    `project-inengine-modes-vision` et `project-codebase-audit-2026-07-07`. Chaque brique :
    extraire derrière un contrat (`*Context`, cf. Editor/Streaming/Npc), build via
    `cmake --build cmake-build-debug --target true-adventurer` (env vcvars64), **ne PAS
    commiter avant validation en jeu par le dev**.

**Leçons de méthode (revue 2026-07-08 — pour ce document et les prochains audits) :**
- **Observations fiables, remèdes = hypothèses.** Trois remèdes proposés par
  l'audit se sont révélés faux à la vérification alors que l'observation était
  juste : la « table monolithique » H-a (aurait violé §2.10), l'option B §2.9
  (router le DoT via `applyDamage` = double-mitigation), le « doublon » U5-1
  (valueRepr ≠ valueToString). Règle : re-vérifier tout remède contre le code
  et le comportement avant de refactorer ; l'observation seule fait foi.
- **Findings perf sans mesure.** U2-01 (`::at` hot path), U7-7/U8-6 (scans
  O(n)) sont des constats statiques, sans profil derrière. Le `FrameProbe`
  existe : mesurer avant d'agir sur ces lignes-là.
- **Filet de tests au fil des extractions.** LandscapeScene n'a aucune
  couverture ; chaque brique repose sur la validation en jeu. Les contrats
  `*Context` rendent les contrôleurs extraits testables headless — quand la
  logique est quasi-pure (machine de fade, `wait`/`rest`), poser un petit TU
  en passant, surtout avant les briques risquées (split renderer).
- **Suivi** : la table §3 porte désormais une colonne **statut** ; la mettre à
  jour à chaque brique commitée (en plus du bloc Batch-3 ci-dessus).

> Note build : le suivi de dépendances de headers de `cmake-build-debug` est bien
> configuré (`CMAKE_CXX_CL_SHOWINCLUDES_PREFIX` détecté en français). Le « ninja: no
> work to do » observé après édition de headers venait d'un décalage de page de code
> du shell `vcvars` utilisé pour l'audit, PAS d'un défaut du projet — les builds CLion
> sont a priori corrects. Aucune modif CMake n'a donc été faite.

---

## 1. Résumé exécutif

Le codebase est **fondamentalement sain et bien stratifié**. Les invariants
porteurs de CLAUDE.md sont vérifiés en place et en bonne santé :

- **Réflexion (keystone §2.3)** : unique mécanisme de (dé)sérialisation, testé
  en profondeur — aucun code de sérialisation ad-hoc par type.
- **Modèle données/§5** : patch field-level, last-writer-wins, save = couche
  de patches — implémenté correctement et couvert par des tests profonds.
- **Abstraction RHI (§2.1)** : zéro appel GL hors backend ; interface
  Vulkan-shaped prête pour un 2e backend.
- **Suite headless (§2.10)** : 76 TUs, déterminisme discipliné, systèmes
  mandatés (resolver, save-layering, GAS) couverts en profondeur.

La dette est **concentrée en deux foyers**, pas diffuse :

1. **`LandscapeScene` god-object** (5914 l., ~12 responsabilités) qui **érode
   le seam extract/submit Phase-5** que tout le reste du codebase protège.
   *(Reste ouvert — Batch 3.)*
2. ~~**Duplication à la frontière de réflexion**~~ **✅ TRAITÉ (Batch 2)** : le
   switch per-`FieldKind` est devenu un `visit` exhaustif (`engine/reflect/
   Visit.hpp`) et la règle de diff §5 vit dans `data::diffToRecord`. Le « clone »
   n'était pas un vrai doublon (opérations distinctes, laissées en place). Voir
   §0 et §H-a/§H-b.

Aucune violation de correction, de save, ou de moddabilité. Les corrections
d'invariant réelles sont peu nombreuses et cadrées (§5 ci-dessous).

---

## 2. Tableau de bord des invariants (§2)

| Invariant | Verdict | Exception (fichier:ligne) |
|-----------|---------|---------------------------|
| §2.1 RHI (aucun GL hors backend) | **PASS** | ImGui via `imgui_impl_opengl3` = exception dev-UI assumée (§3) — `engine/ui/ImGuiLayer.cpp:4` |
| §2.2 Forms vs References | **PASS** | References = `ReferenceForm` ; place/move/disable = patches (U7) |
| §2.3 Réflexion keystone | **PASS** | 28+ types réfléchis, zéro sérialisation ad-hoc |
| §2.4 Une couche patches (mods+saves) | **PASS** | `resolve()` = load order plat, save appended last (U7) |
| §2.5 Identité par GUID | **PASS** | `FormHandle` = index runtime ; identité persistante = `core::Guid` (U7) |
| §2.7 Spawner par catégorie | **PASS** | dispatch + wiring 100 % réflexion, agrégés (`Spawner.cpp:264`) |
| §2.8 État script en composant réfléchi | **PASS** | `ScriptVars` réfléchi ; VM unique, `self`=proxy (U8) |
| §2.9 Attributs via GameplayEffect | **PASS-with-exception → MEDIUM** | Pipeline test-locké ; déviations réelles = sites tick périodique (regen/DoT/mort-buildup) : `CharacterTick.cpp:74-78,119` + `GameTime.cpp:22-27,49,89-90`. **`Damage.cpp:81` et `Spawner.cpp:103` NE SONT PAS des violations** (exec-calc / init seeding — voir DEEP-attribute-mutation.md) |
| §2.10 Pureté headless — `data/` | **PASS** | zéro include rhi/render |
| §2.10 Pureté headless — `world/` | **PASS-with-exception** | 1 letter-violation : `world/terrain/TerrainPatches.hpp:8` inclut `engine/render/landscape/TerrainNoise.hpp` (symboles importés headless-purs) |
| §2.10 Pureté headless — link-level | **NON VERROUILLÉ** | aucune cible de link prouvant sim-sans-render (`tests/CMakeLists.txt:78`, U9-3) |
| §3.1 Headers platform-clean | **PASS** | aucun `windows.h`/X11/Wayland/SDL/glad dans un `.hpp` ; natifs derrière pimpl (U1) |
| Façades seam pimpl (Jolt/miniaudio/RmlUi) | **PASS** | aucun type tiers ne traverse `Physics.hpp`/`Audio.hpp`/`UiSystem.hpp` ; deps PRIVATE (U8) |
| flecs confiné à `meadows-ecs` | **FAIL partiel** | `flecs.h`+`flecs::entity` fuient dans `gameplay/save/SaveState.hpp:3` et `gameplay/actors/CharacterTick.hpp` (U8-2) |

---

## 3. Table maîtresse des findings

Triée par sévérité (crit→high→med→low) puis effort (S d'abord) — les gains
rapides remontent dans chaque bande. Sévérité §2.9 : **F1 rétrogradé HIGH→MEDIUM**
par le deep-dive.

**Statut** : ✅ traité (commit en §0) · ◐ en cours (Batch 3) · ✖ acté sans
action (exception assumée / non-violation) · vide = ouvert. ⚠️ Les ancres
`fichier:ligne` datent du 2026-07-07 (avant remédiation) — LandscapeScene a
déjà perdu ~1000 lignes ; se fier aux noms de symboles, pas aux numéros.

| id | statut | unité | sév | axe | fichier:ligne | description | effort | inter |
|----|:------:|-------|-----|-----|---------------|-------------|--------|-------|
| U4-1 | ◐ | U4 | crit | archi/factor | LandscapeScene.hpp:77 / .cpp:87-5914 | God-object : 1 classe agrège ~12 responsabilités | L | non |
| U1-01 | ✅ | U1 | high | archi/qual | reflect/Reflect.hpp:39-77 | Triple ordinal FieldKind/Value/KindOf = contrat binaire, sans static_assert | S | oui |
| U3-2 | ✅ | U3 | high | réutil | TerrainNoise.cpp:10 (+5 fichiers) | murmur3 `hashU32` copié dans 6 fichiers | S | oui |
| U7-1 | ✅ | U7 | high | archi | world/terrain/TerrainPatches.hpp:8 | `world/` inclut `engine/render/` (letter-violation §2.10) | S | oui |
| U4-3 | ✅ | U4 | high | factor | LandscapeScene.cpp:87-707 | `onEnter` = 620 l. séquentielles, ré-entrantes | M | non |
| U4-5 | ✅ | U4 | high | archi/factor | LandscapeScene.cpp:1592-1863 | Éditeur+sculpt embarqués dans la scène de jeu (double EditorScene) | M | oui |
| U5-2 |   | U5 | high | factor | game/TextureCache.* ; MeshCache.* | Machinerie async-residency dupliquée entre 2 caches | M | non |
| U7-2 | ✅ | U7 | high | factor | BinaryFormat.cpp:45 ; TomlWriter.cpp:25 ; PluginLoader.cpp | Switch per-FieldKind répliqué (H-a) | M | oui |
| U8-3 | ✅ | U8 | high | factor | tools/cooker/Main.cpp:189-205 | Liste `registerXxxFormTypes` recopiée à la main (déjà cause d'un bug) | M | oui |
| U3-1 | ✅ | U3 | high | factor | GrassSystem.cpp:244 ; TerrainSystem.cpp:223 ; VegetationSystem.cpp:305 | Ring chunk-streaming implémenté 3× | L | non |
| U4-2 | ✅ | U4 | high | archi | LandscapeScene.cpp:5217,4531,3839 | Seam Phase-5 contourné : `render()` lit le World vivant | L | oui |
| U4-4 | ✅ | U4 | high | qual | LandscapeScene.cpp:707-858 / hpp:195-663 | ~40 handles GPU bruts, miroir onExit fragile (§8 RAII) | L | oui |
| U5-1 | ✅ | U5 | high | factor | PropertyGrid.cpp:69-258 ; EditorScene.cpp:34 | Switch FieldKind écrit 3× (face U5 de H-a) | L | oui |
| U1-02 | ✅ | U1 | med | qual | reflect/Registry.cpp:7-14 | Collision type-id loggée non assertée ; 2e type droppé | S | non |
| U1-04 | ✅ | U1 | med | réutil | core/FrameProbe.hpp:20 (+épars) | Aucun clock primitive partagé ; std::chrono re-dérivé 4+ sites | S | oui |
| U3-4 | ✅ | U3 | med | factor | TerrainSystem.hpp:112 (+15 sites) | Pack/unpack clé u64 chunk à la main | S | non |
| U3-5 | ✅ | U3 | med | factor | TerrainSystem.cpp:371 (+5 pipelines) | Layout attributs `MeshVertex` réécrit ~5× | S | non |
| U4-8 | ✅ | U4 | med | factor | LandscapeScene.cpp:1128-1178,4936 | Crossfade météo lerpe ~18 champs à la main, listés 3× | S | oui |
| U5-6 |   | U5 | med | qual | game/ui/PropertyGrid.cpp:20 | `ActiveEdit gActive` = global mutable (§8) | S | non |
| U7-3 | ✅ | U7 | med | archi | BinaryFormat.cpp:14,45 ; Reflect.hpp:39 | Stream binaire dépend de l'ordre numérique de l'enum FieldKind | S | oui |
| U8-1 | ✅ | U8 | med | réutil | quest/Quest.cpp:15 ; Dialogue.cpp:14 | `forEachForm<T>` réimplémenté, duplique `data::forEach` | S | oui |
| U1-03 |   | U1 | med | réutil | (core, absence) | Aucun type Result/expected (§8) ; optional+log perd la raison | M | oui |
| U2-01 |   | U2 | med | qual | GlDeviceBase.cpp:134-312 | Hot path draw utilise `unordered_map::at` (exceptions + hash/draw) | M | non |
| U2-02 |   | U2 | med | réutil | Rhi.hpp:25-31 | 7 handle structs quasi-identiques, aucun type partagé (H-d) | M | oui |
| U3-3 | ✅ | U3 | med | archi/qual | FrameUniforms.hpp:13 vs common.glsl:3 | Struct C++ ↔ bloc GLSL synchro par commentaire seul (H static_assert) | M | oui |
| U3-6 | ✅ | U3 | med | factor | PostFx.cpp:199,81 | Targets half-res + somme shaderGeneration écrits en double | M | non |
| U4-6 | ✅ | U4 | med | archi | LandscapeScene.cpp:4982-5596 | `render()` = 615 l. illisibles | M | non |
| U4-7 |   | U4 | med | propreté | LandscapeScene.cpp (~609 littéraux) | Constantes magiques hors Forms de tuning | M | non |
| U4-9 | ✅ | U4 | med | factor | LandscapeScene.cpp:2589,3310 | ~10 méthodes push-modèle RmlUi mêlées à la logique jeu | M | non |
| U5-3 | ✅ | U5 | med | factor | CombatArenaScene.cpp:190 ; DemoScenes.cpp:347 | Enregistrement runtime gameplay-tags copié par scène | M | oui |
| U5-5 | ✅ | U5 | med | archi | game/SaveGame.cpp:24 vs :58 | 2 chemins de capture reference divergent en jeu de champs (H-f) | M | oui |
| U6-F2 | ✅ | U6 | med | factor | CharacterTick.cpp:73 vs GameTime.cpp:19 | `applyBuildupResult` dupliqué ET divergent (H-e) | M | oui |
| U6-F3 | ✅ | U6 | med | factor | GameplayEffects.cpp:302 vs :352 | 2 boucles tick quasi-identiques (H-e) | M | oui |
| U6-F4 |   | U6 | med | réutil | event/EventBus.hpp:36 ; cue/GameplayCues.hpp:41 | EventBus + CueRegistry = 2 dispatch parallèles (H-c) | M | oui |
| U6-F5 | ✅ | U6 | med | factor | save/SaveState.hpp:33 | Clone/diff réflexion duplique resolver/EditSession (H-b) | M | oui |
| U6-F7 | ✅ | U6 | med | qual | Combat.cpp:5 ; CharacterTick.cpp:99 ; GameTime.cpp:47 | Détection de mort incohérente (base vs current) | M | non |
| U6-F10 | ✅ | U6 | med | archi | GameTime.cpp:131 | `recomputeCurrent` 2-arg laisse les derived transitoirement faux | M | non |
| U7-4 | ✅ | U7 | med | factor | EditSession.cpp:12,193 ; Synthesis.cpp | Clone/diff réflexion dupliqué (H-b) | M | oui |
| U8-2 | ✅ | U8 | med | archi | gameplay/save/SaveState.hpp:3 ; CharacterTick.hpp | flecs fuite dans headers gameplay | M | oui |
| U8-4 | ✅ | U8 | med | qual | script/Vm.cpp:161,265 | Coroutines gardent pointeurs bruts, dangling si entité meurt | M | non |
| U9-1 | ✅ | U9 | med | qual | tests/SceneSubmitTest.cpp:1-40 | Seam RenderSnapshot à peine testé (2 cas) | M | oui |
| U9-2 | ✅ | U9 | med | factor | tests/CookerTest.cpp:18 | Aucun golden test figeant les ordinaux réflexion réels | M | oui |
| U9-3 | ✅ | U9 | med | archi | tests/CMakeLists.txt:78 | Aucune cible link prouvant sim-sans-render (§2.10) | M | oui |
| U6-F1 | ✅ | U6 | med | archi | CharacterTick.cpp:74-78,119 ; GameTime.cpp:22-27,49,89-90 | §2.9 : regen/DoT/mort-buildup écrivent la base hors pipeline (voir DEEP) | L | oui |
| U4-10 | ✅ | U4 | med | archi/factor | LandscapeScene.cpp:1014,4195 | `update()` orchestre 10 sous-systèmes ; NPC+interaction inline | L | non |
| U5-4 |   | U5 | med | factor | EditorScene.cpp:128-704 | 3 éditeurs dev réimplémentent la même forme (~700 l.) | L | non |
| U1-05 | ✅ | U1 | low | réutil | fx/Particles.cpp:10-26 | `hashU32` dupliqué avec landscape scatter | S | oui |
| U1-07 |   | U1 | low | propreté | Window.cpp:46 ; GlContext.cpp:43 | `new` littéral dans factories à ctor privé | S | non |
| U1-08 | ✅ | U1 | low | archi | assets/GltfMesh.hpp:8 | Loader asset dépend de `render::MeshData` | S | oui |
| U1-09 | ✅ | U1 | low | propreté | _old/renderer_test/ | Arbre dead-code au racine | S | oui |
| U2-05 | ✅ | U2 | low | propreté | GlDeviceBase.cpp:46-63 | Compile compute mislabellé "fragment" dans le log | S | non |
| U2-06 | ✅ | U2 | low | factor | GlDevice41.cpp:9 ; GlDevice46.cpp:11 | Helpers libres re-déclarés à la main par sous-classe | S | non |
| U2-07 | ✖ | U2 | low | archi | engine/ui/ImGuiLayer.cpp:4 | ImGui bypasse le RHI (exception assumée) | S | oui |
| U2-08 | ✅ | U2 | low | propreté | Rhi.hpp:44 | Commentaire "(later brick)" périmé (copyTexture implémenté) | S | non |
| U2-09 | ✅ | U2 | low | propreté | _old/renderer_test/ | Renderer pré-RHI mort dans l'arbre | S | oui |
| U3-8 |   | U3 | low | propreté | FrameUniforms.hpp:17 vs common.glsl:7 | Dérive de commentaires entre les 2 miroirs | S | non |
| U4-11 |   | U4 | low | propreté | LandscapeScene.cpp:3558,2558 | Strings UI en français codées en dur (§8 anglais) | S | non |
| U4-12 |   | U4 | low | qual | LandscapeScene.cpp:243,5210 | Structs GPU inline dans corps de fonction | S | oui |
| U4-14 |   | U4 | low | propreté | LandscapeScene.cpp:1197,5119 | Commentaires-fossiles post-refactor | S | non |
| U5-7 | ✅ | U5 | low | propreté | game/TextureCache.cpp:59-63 | Bloc de commentaire collé en double | S | non |
| U5-8 |   | U5 | low | factor | CombatArenaScene.cpp:290 ; DemoScenes.cpp:222 | WASD→vecteur réécrit par scène | S | oui |
| U5-9 |   | U5 | low | propreté | game/scenes/DemoScenes.* | 523 l. groupent 6 classes de scène | S | non |
| U5-10 |   | U5 | low | qual | EditorScene.cpp:97-108 | `reload()` resolve sync sur thread UI | S | non |
| U6-F6 |   | U6 | low | réutil | EventBus.hpp:34 ; GameplayCues.hpp:46 ; AbilitySystem.hpp:37 | 3 schémas ad-hoc `nextId` (H-d) | S | oui |
| U6-F8 | ✅ | U6 | low | qual | ability/AbilitySystem.hpp:74 | `setCurrentValue` public bypasse recompute (footgun, test-only) | S | non |
| U6-F9 | ✅ | U6 | low | propreté | AbilitySystem.hpp:43 (+4) | Commentaires "deferred to Phase 8" périmés (save existe) | S | non |
| U7-5 | ✖ | U7 | low | archi | world/scene/Spawner.cpp:103 | Spawner seed via setBaseValue direct (init sanctionné) | S | non |
| U7-8 |   | U7 | low | propreté | world/scene/Spawner.cpp:10 | Spawner tire ~12 headers gameplay/stats | S | non |
| U8-5 | ✖ | U8 | low | archi | engine/ui/ImGuiLayer.cpp:4 | ImGui backend GL propre hors RHI (toléré) | S | oui |
| U8-7 | ✅ | U8 | low | qual | script/Vm.cpp:252 | Handler Lua avale les erreurs (échec silencieux) | S | non |
| U8-9 | ✅ | U8 | low | réutil | script/Vm.cpp:26-59 | `valueToLua`/`luaToValue` = chaîne if-constexpr par kind (H-a) | S | oui |
| U8-10 | ✅ | U8 | low | propreté | tools/cooker/Main.cpp:172 | `new-guid count` via atoi non borné | S | non |
| U9-4 |   | U9 | low | factor | tests/*.cpp (~147 occ) | `makeTypes()`/guid/TOML recopiés dans ~30 TUs | S | non |
| U9-5 | ✅ | U9 | low | qual | tests/GameClockTest.cpp:1 | Seam GameClock→effets game-time non couvert (H-e) | S | oui |
| U9-6 |   | U9 | low | couverture | tests/QuestTest.cpp | Aucun cas d'alias quête | S | non |
| U9-7 |   | U9 | low | couverture | game/ui/PropertyGrid.cpp | Value↔widget sans assertion propre (H-a) | S | oui |
| U9-8 |   | U9 | low | couverture | tests/CuesSchedulesTest.cpp:175 | Usage mobilier (begin/end use) non testé | S | non |
| U1-06 |   | U1 | low | qual | core/Jobs.hpp:14-26 | Durée de vie JobCounter comment-only ; dangling ref UB | M | non |
| U4-13 |   | U4 | low | réutil | LandscapeScene.hpp:522,417 | Structs runtime (Npc/MeshDraw/…) dans le header de scène | M | non |
| U7-6 | ✅ | U7 | low | qual | Resolver.cpp:78 ; Record.hpp:35 | Load order (dependencies) non validé | M | non |
| U7-7 |   | U7 | low | reuse | data/forms/FormQuery.hpp:23,38,67 | Scans FormQuery non indexés (O(n)) | M | non |
| U8-6 |   | U8 | low | qual | quest/Quest.cpp:48 ; Dialogue.cpp:65 | Scans O(total forms) par événement quête/dialogue | M | non |
| U8-8 |   | U8 | low | factor | engine/ui/UiSystem.hpp:47 | `UiModelEventHandler` = 3e canal dispatch (H-c) | M | oui |
| U1-10 |   | U1 | low | qual | reflect/Reflect.hpp:94-98 | `std::function` per-field sur hot path save/diff | L | non |
| U2-03 | ✅ | U2 | low | factor | GlDevice41.cpp:105 ; GlDevice46.cpp:256 | `createPipeline` copie 8 champs identiques dans 2 backends | S | non |
| U2-04 |   | U2 | low | archi | Rhi.hpp:139-162 | `ShaderDesc` porte du GLSL brut (dette Vulkan-readiness) | L | non |
| U3-7 | ✅ | U3 | low | qual | PostFx.cpp:107 ; VegetationSystem.cpp:268 | Free RHI par listes manuelles paires (leak invisible) | L | oui |
| U3-9 | ✖ | U3 | note | archi | ChunkOcclusion.hpp:31 ; GpuOcclusion.hpp:30 | Double producteur d'occlusion (staging intentionnel) | — | oui |

> Note : U2-03 est effort S mais sévérité low (placé après les low-M/L par tri
> sévérité-d'abord — corrigé ici à titre indicatif). Le tri principal reste
> sévérité → effort.

---

## 4. Primitives à mutualiser (le cœur de la synthèse)

La réponse à « qu'est-ce qui est factorisable / réutilisable par plusieurs
systèmes ? ». Chaque item nomme **tous les call-sites** qu'il unifierait.

### H-a — Dispatch `reflect::Value` unique ✅ FAIT (`4ac53a5`, `f2865bd`)
**⚠️ Suggestion d'origine AMENDÉE — ne pas ré-appliquer la « table ».** La
proposition initiale (une table `FieldKind → {writeBin, writeToml, editWidget,
toLua, …}`) a été **écartée** : elle couplerait le binaire (`data/`) à ImGui
(`game/`) et Lua (`script/`) dans une seule structure → **violation §2.10**.
Réalisé à la place : `engine/reflect/Visit.hpp` (`overloaded` + `visit`), un
*mécanisme* de dispatch exhaustif (un kind oublié = erreur de compil), chaque
site gardant son propre corps. Sites convertis :
- `BinaryFormat.cpp` (writer), `TomlWriter.cpp` (**U7-2**) — octets identiques.
  Le **Reader reste un switch** : il construit une `Value` depuis les octets
  (rien à visiter) et échoue déjà proprement sur un kind inconnu.
- `PropertyGrid.cpp` (valueToString + drawPropertyGrid), `EditorScene.cpp`
  (valueRepr) (**U5-1**) — `valueFromString` reste un switch (construit depuis
  un kind externe). Constat : valueRepr ≠ valueToString (affichage décoré vs
  texte re-parsable), **pas** le doublon supposé.
- `Vm.cpp` (`valueToLua`) (**U8-9**) — `luaToValue` reste un switch (idem Reader).

### H-b — Règle §5 « record = seulement ce qui change » en un point ✅ FAIT (`40c9fcf`)
Réalisé : `data::diffToRecord(type, object, reference, record, includeInherited)`,
partagé par `EditSession::exportPlugin` (**U7-4**) et `SaveState::createRecord`
(**U6-F5**). Le flag `includeInherited` préserve la seule vraie différence
(éditeur = parents inclus pour `editorId` ; save = own-only).
**⚠️ `cloneFields`/`copyMatchingFields` NON extraits** (contrairement à la
suggestion) : ce sont deux opérations distinctes (clone same-type vs cross-type
par nom+kind) avec **un seul appelant chacune** — les déplacer n'enlèverait
aucune duplication (§10). Laissés en place. Synthesis assemble des champs
explicitement choisis (pas un diff) — hors périmètre. Le crossfade météo
(`LandscapeScene.cpp:1128`, **U4-8**) reste un cas `lerpFields` non traité.

### H-c — Une primitive `Signal<Payload>` de dispatch
Alloc-id ordonnée + dispatch + ré-entrance, sur laquelle bâtir les 3 canaux :
- `gameplay/event/EventBus.hpp:36` (bus multi-abonnés) (**U6-F4**)
- `gameplay/cue/GameplayCues.hpp:41` (CueRegistry) (**U6-F4**)
- `engine/ui/UiSystem.hpp:47` (`UiModelEventHandler`, callback unique) (**U8-8**)

Sources : **U6, U8**.

### H-d — Un strong-typedef `Handle<Tag>` (id + valid() + ==)
Remplace les structs `{u32 id}` et allocateurs `nextId` dispersés :
- 7 handle structs RHI `Rhi.hpp:25-31` (**U2-02**)
- `FormHandle` (data), `BodyId` (physics), `SubscriptionId`/cue handle/`effectId`
  `EventBus.hpp:34`,`GameplayCues.hpp:46`,`AbilitySystem.hpp:37` (**U6-F6**)

Sources : **U2, U6**.

### hashU32 / murmur3 + HashRng → `engine/core/Hash.hpp`
Le finaliseur murmur3 identique (constantes `0x7feb352d`/`0x846ca68b`) est
copié dans **6+ fichiers** ; `HashRng` dans 3 :
- `fx/Particles.cpp:10-26` (**U1-05**)
- `render/landscape/` : `TerrainNoise.cpp:10`, `GrassSystem.cpp:35`,
  `VegetationSystem.cpp:21`, `SplatTextures.cpp:13`, `TreeGenerator.cpp:11`,
  `MeshBuilder.cpp:10` (**U3-2**)

Home naturel : `engine/core/Hash.hpp` à côté de `fnv1a`. Sources : **U1, U3**.

### Enregistrement des Form-types : source de vérité unique
`registerAllFormTypes(registry)` existe mais la liste est recopiée à la main
ailleurs (déjà cause d'un bug « records droppés ») ; il **manque** aussi les
agrégateurs composants/tags/spawners :
- `tools/cooker/Main.cpp:189-205` (commentaire « Keep in sync with EditorScene ») (**U8-3**)
- `game/scenes/EditorScene.cpp::onEnter`, `AllForms` (**U7/U5**)
- runtime gameplay-tags copiés par scène : `CombatArenaScene.cpp:190`,
  `DemoScenes.cpp:347`, LandscapeScene → `registerRuntimeGameplayTags()` manquant (**U5-3**)

Sources : **U5, U7, U8**.

### Template `ResidencyCache<Payload>`
`game/TextureCache.*` et `game/MeshCache.*` dupliquent toute la machinerie
async (queue, generation, pending, Residency enum, resolve→placeholder→enqueue,
pump-drain, no-wait dtor) ; seuls décode + upload diffèrent (**U5-2**). Source : **U5**.

### `ChunkStreamer<Instance>` (ring de streaming)
Le ring budgeté (nearest-first `Candidate{cx,cz,dist2}`, evict-hystérésis,
minY/maxY, generation) implémenté 3× dans `GrassSystem.cpp:244` /
`TerrainSystem.cpp:223` / `VegetationSystem.cpp:305` ; absorbe aussi le
pack/unpack clé u64 (**U3-4**). Source : **U3-1**.

### Static-assert guards pour les contrats on-disk / GPU
- **Ordinal FieldKind** : `variant index == FieldKind == KindOf` — contrat
  binaire de chaque plugin cuit + save, gardé par commentaire seul
  (`reflect/Reflect.hpp:39-77`, `BinaryFormat.cpp:14,45`) — **U1-01, U7-3** ;
  + golden ordinal test manquant (**U9-2**).
- **FrameUniforms ↔ common.glsl** : struct C++ ↔ bloc GLSL synchro par
  commentaire (`FrameUniforms.hpp:13` vs `shaders/common.glsl:3`) — un membre
  désaligné corrompt ~14 shaders — **U3-3** (rempli par U4).

Sources : **U1, U7, U9** (binaire) ; **U3, U4** (GPU).

### Primitives core manquantes
- **`core::Result<T>`/`Status`** — §8 mandate `std::expected` ; introuvable hors
  CLAUDE.md. De-facto `optional`+`LOG_*` jette la raison de l'erreur (**U1-03**).
- **`core::Clock`** — pas d'alias steady_clock/`nowMs()` ; `std::chrono` re-dérivé
  dans 4+ sites (FrameProbe, Engine, TerrainSystem, UiSystem, SaveGame) (**U1-04**).

---

## 5. Violations d'invariant à corriger

Les vraies, avec la nuance du deep-dive §2.9 :

1. **flecs fuit dans des headers gameplay** (§3 « confiné à meadows-ecs »).
   `gameplay/save/SaveState.hpp:3` et `gameplay/actors/CharacterTick.hpp`
   incluent `flecs.h` + exposent `flecs::entity` (**U8-2**). À trancher côté U6 :
   abstraction ecs ou déplacement des signatures. Effort M.

2. **§2.10 letter-violation : `world/` inclut `engine/render/`.**
   `world/terrain/TerrainPatches.hpp:8` inclut `TerrainNoise.hpp` (symboles
   headless-purs mais mal placés sous `render/`). Relocaliser `HeightPatch`/
   `HeightPatches` vers un home headless (**U7-1**). Effort S. **Corollaire** :
   le seam n'est **pas verrouillé au link** — aucune cible prouve sim-sans-render
   (**U9-3**). Effort M.

3. **§2.9 tick-loop regen / DoT / mort-buildup — MEDIUM** (voir
   `DEEP-attribute-mutation.md`). Sites réels : `CharacterTick.cpp:74-78,119`
   (DoT buildup + regen énergie) et `GameTime.cpp:22-27,49,89-90` (DoT + mort +
   regen game-time) écrivent `vitals.*` (base) hors `applyEffect`. **Le pipeline
   d'effet lui-même est correct et test-locké** ; ces écritures de base
   préservent le contrat de save. `Damage.cpp:81` (exec-calc) et `Spawner.cpp:103`
   (init seeding) **ne sont PAS des violations**. Remède : router DoT/mort via
   `applyDamage` (S) + soit sanctionner le regen comme execution-calc dans §2.9
   (S), soit bâtir des effets périodiques à magnitude-capturée (M-L).

4. **Seam Phase-5 contourné dans `LandscapeScene::render`** (**U4-2**).
   `render()`/`draw*()` interrogent le **World vivant** (`collectLights`,
   queries `LightSource`/`WaterVolume`/`MeshRender`, pointeurs `Npc`) au lieu de
   consommer un `RenderSnapshot` — seuls les meshes sont extraits. **Le type
   `RenderSnapshot` lui-même est propre** (U5 : POD auto-possédant vérifié
   ligne-à-ligne) ; c'est la scène qui le bypasse. Étendre le snapshot
   (lights/poses NPC/water/shafts) ; `render()` ne lit que le packet. Effort L.

---

## 6. Verdict de santé par unité

| Unité | Verdict | 1-ligne |
|-------|---------|---------|
| U1 Fondations & keystone | **Sain** | La partie la plus saine ; primitives RAII-propres, invariants tenus ; gaps = garde-fous absents |
| U2 RHI + backend GL | **Sain** | Interface Vulkan-shaped réelle ; split 41/46 propre ; résidus factor + hot-path `at` |
| U3 Renderer paysage | **Correct** | 14 systèmes cohésifs, zéro GL fuité ; dette = 3 duplications transverses (streaming/hash/UBO) |
| U4 LandscapeScene | **À risque** | God-object 5914 l., ~12 responsabilités, seam Phase-5 érodé — foyer de dette n°1 |
| U5 Runtime/scènes/UI | **Correct** | Seam RenderSnapshot PASS vérifié ligne-à-ligne ; leviers = factor (H-a, caches, éditeurs) |
| U6 GAS + stats | **Correct** | Pureté/RNG/réflexion PASS ; §2.9 tick MEDIUM ; convergences H-b/H-c/H-d/H-e |
| U7 Data + monde | **Sain** | §5/§2.5/§2.7 exemplaires, propreté excellente ; 1 letter-violation + refactors |
| U8 Narratif/script/seams | **Sain** | §2.8 au mot, façades étanches, cooker réutilise le pipeline ; hotspots = liste form-types + coroutines |
| U9 Tests | **Sain** | Systèmes mandatés couverts en profondeur, déterministe ; trous = seam snapshot, ordinaux, link headless |

---

## 7. Notes d'état

- **`_old/renderer_test/`** : arbre dead-code au racine (raw GL/shader pré-RHI),
  pollue chaque scan engine-wide. À supprimer (**U1-09, U2-09**).
- **Chantier-6 axe B (lighting)** : la MEMORY signalait ce travail **non commité**
  dans le working tree. **Vérifié au moment de l'audit : `git status` propre** —
  le lighting est désormais commité. (Confirmer avant tout ré-audit du lighting.)
- **Strings UI en français codées en dur** dans `LandscapeScene`
  (`"Crime observe ! Prime : …"`, cpp:3558) alors que les `LOG_*` sont en
  anglais (§8). Sortir les strings joueur en données localisables (**U4-11**).

---

## 8. Feuille de route de remédiation suggérée

**Proposition à trier par le dev** — cadence brique-par-brique, aucune brique
ne modifie de code du fait de l'audit. Trois batches par risque croissant.

### Batch 1 — « gains rapides » (effort S, faible risque)
Quick wins isolés, chacun une brique validable :
- Supprimer `_old/renderer_test/` (**U1-09/U2-09**).
- Supprimer/rendre test-only le footgun `setCurrentValue` (**U6-F8**).
- `static_assert` guards : ordinal FieldKind (**U1-01/U7-3**) + golden ordinal
  test (**U9-2**) ; FrameUniforms↔common.glsl (**U3-3**, partie assert).
- Déplacer `HeightPatch`/`HeightPatches` hors `render/` → `world/` (**U7-1**).
- Corriger la fuite flecs des headers gameplay (**U8-2**, si S par déplacement
  de signature ; sinon Batch 3).
- Nettoyer commentaires périmés/fossiles (**U2-08, U6-F9, U4-14, U5-7, U3-8**).
- Hoister `hashU32`/`HashRng` → `core/Hash.hpp` (**U1-05/U3-2**).
- ~~Router DoT-buildup + mort-buildup via `applyDamage`~~ (§2.9 DEEP §3 option B)
  — **REJETÉ**. La vérification a montré que router le DoT via `applyDamage`
  double-mitige (la résistance agit sur le *seuil* de buildup, pas sur le dégât
  par tick). Résolu par **Option A** : les écritures de tick sont des
  « execution calculations » sanctionnées (CLAUDE.md §2.9, `a51fae7`). Voir §0.

### Batch 2 — « mutualisation reflect » ✅ FAIT (`4ac53a5`, `f2865bd`, `40c9fcf`)
Voir les sections détaillées **H-a** et **H-b** ci-dessus pour ce qui a réellement
été livré (et les suggestions amendées : pas de table monolithique, pas de
`cloneFields`). Résumé : dispatch `reflect::Value` exhaustif (`engine/reflect/
Visit.hpp`) sur BinaryFormat/TomlWriter/PropertyGrid/EditorScene/Vm
(**U7-2, U5-1, U8-9**) ; règle §5 diff en un point (`data::diffToRecord`) pour
EditSession + SaveState (**U7-4, U6-F5**). **U8-3** (agrégateur form-types) fait
séparément (`3a02d8e`). Restent ouverts : agrégateur tags/spawners (**U5-3**),
lerpFields météo (**U4-8**), primitives core `Result`/`Clock` (**U1-03/U1-04**).

### Batch 3 — « structurel » (effort L, à séquencer)
- **Décomposition `LandscapeScene`** selon la proposition U4 (§Décomposition) :
  commencer par WeatherController + SceneEditor/SculptTool (isolés), puis GameHud
  + NpcDirector/InteractionController, garder **LandscapeRenderer + extension du
  RenderSnapshot pour la fin** (**U4-1, U4-3, U4-5, U4-6, U4-9, U4-10**).
- **Restauration du seam Phase-5** : étendre `RenderSnapshot`
  (lights/poses/water/shafts) ; prérequis au split renderer (**U4-2/U4-4**).
- Consolidations **H-c** (`Signal<Payload>`) et **H-d** (`Handle<Tag>`)
  (**U6-F4/U8-8, U2-02/U6-F6**).
- Templates `ResidencyCache` (**U5-2**) et `ChunkStreamer` (**U3-1**).
- Wrapper RAII GPU owning (supprime les miroirs onExit / free-lists manuels :
  **U4-4, U3-7**).
