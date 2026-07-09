# Chantier 8 — Outillage

> **REPRIS 2026-07-09 (décision dev) : le chantier n'était PAS fini.**
> Les briques 8.1-8.5 (ci-dessous) ont livré des éditeurs *arborescents* ;
> la vision du chantier est **une interface visuelle pour toutes les
> fonctionnalités, notamment des systèmes de présentation nodals**. Les
> briques 8.6-8.11 (plan en fin de document) livrent : graphes nodaux
> (anim/quêtes/dialogues), timeline des events de clip, builder de
> conditions, éditeur FX à preview live, éditeurs Effect/Ability.

> **Briques 8.1-8.5 FAITES (2026-07-07) — exécutées d'une traite sur le
> go du dev.** 279 tests verts, smoke-run par brique, builds Debug +
> Release. Validation à la main des panneaux par le dev en attente
> (scène « Game DB (editor) » du hub Demos). Nouvelles APIs :
> `EditSession::duplicateForm`/`forEachVisible`, `data::referencesTo`,
> `FieldConflict.writers` = `FieldWrite{plugin, value}` (+typeId/
> fieldId), `data/plugins/Synthesis` (makeSynthesisPatch +
> writeSynthesisToml). Leçon re-payée : FieldConflict est un type
> partagé → purge `*.dir` + rebuild complet (fait, préventivement,
> aussi pour EditSession).

> Plan écrit 2026-07-07 (go dev). Objectif : accélérer l'AUTHORING du
> contenu de la démo. Tout outil est un panneau ImGui de l'EditorScene
> posé sur l'infra existante — `EditSession` (drafts, undo, export =
> plugin), PropertyGrid, records décomposés liés par id (Phase 4).
> Aucun nouveau mécanisme moteur : un éditeur ÉCRIT des plugins (§5).

## Contexte

- Le runtime des quêtes/dialogues/schedules est complet et prouvé en jeu
  (Phase 4 + chantier 6 axe A « La menace de l'est ») ; il manque les
  outils — aujourd'hui tout s'écrit en TOML à la main.
- Le GameDB browser (H2) et le PluginsPanel (load order + rapport de
  conflits par champ) existent ; l'EditSession exporte déjà un plugin
  diff (records triés par guid).
- Les éditeurs anim / FX / UI restent P1 avec un P0 assumé (TOML à la
  main + hot-reload) → HORS CHANTIER, stretch si tout le reste est plié.

## Les briques (par valeur)

### 8.1 — GameDB confort : duplication + « utilisé par »
- **Dupliquer un record** : nouveau guid minté par l'EditSession, tous
  les champs copiés par réflexion, editorId suffixé ; les enfants ne
  sont PAS dupliqués (v1 — dupliquer une quête entière est le job de
  l'éditeur de quêtes).
- **Cross-références** : panneau « utilisé par » — scan par réflexion de
  tous les champs Guid de la base résolue qui pointent le form
  sélectionné (type + editorId + champ), navigation au clic.
- Headless : les deux opérations (clone par réflexion, scan) doctestées.

### 8.2 — Éditeur de quêtes
- Arbre Quest → States → Branches → Tasks (childrenOf, le pattern §C.1),
  sélection → PropertyGrid ; badges kind (Regular/Success/Failure) et
  startState.
- Création guidée : « + state / + branch / + task » avec le parent
  pré-rempli ; suppression = retrait du draft (les records base restent).
- Validation affichée : startState nul, branch sans destination, task
  sans event → warnings inline.
- Export : le flux session→plugin existant (rien de neuf).

### 8.3 — Éditeur de dialogues
- Arbre DialogueNodeForm par parent/order (indentation = profondeur),
  texte édité inline, speaker/event visibles ; « + réponse / + suite »
  avec parent et order pré-remplis ; re-order par boutons ↑↓ (échange
  des champs order).
- Le nœud sélectionné montre ses conditions (lecture seule v1 — le
  ConditionForm s'édite au PropertyGrid).

### 8.4 — Timeline des schedules
- Vue 24 h par ScheduleForm : une barre par ScheduleEntryForm
  (startHour→endHour, wrap minuit rendu en deux segments), libellé =
  package ; clic = sélection → PropertyGrid ; drag des bords = retune
  des heures (write-through EditSession, undo gratuit).
- « + entry » au clic sur un trou ; scrub d'heure (slider) réutilisant
  `evaluateSchedule` pour prévisualiser le slot actif (l'infra
  drawSchedules H7).
- La règle « last-in-load-order wins » visualisée : entrées empilées
  dans l'ordre, la gagnante opaque, les recouvertes hachurées.

### 8.5 — Outil de synthesis patch (§5.1 CLAUDE.md)
- **Prérequis resolver** : `FieldConflict` porte la VALEUR de chaque
  écrivain (repr texte du champ au moment de l'apply), pas seulement
  son nom. Doctest.
- PluginsPanel, vue conflits : par champ en conflit, choix du gagnant
  (radio par écrivain, valeurs affichées) ou valeur custom.
- « Générer le patch » : plugin ordinaire émis par TomlWriter — champs
  arbitrés uniquement, `dependencies` = les mods arbitrés, provenance
  en métadonnée (`# from: <plugin>`) pour la régénération.
- Invariant §5 : AUCUN mécanisme parallèle — la sortie est une couche
  de plus, chargée en dernier. Doctest : génération + re-résolution =
  zéro conflit restant sur les champs arbitrés.

### Stretch (si le chantier se plie vite)
- Duplication d'une quête entière (clone récursif des enfants).
- QoL éditeur de niveau : duplication de référence sélectionnée, snap
  de grille (restes chantier 2).
- Éditeur de FX (panneau live sur ParticleForm).

## Quoi tester (dev)

Scène « Game DB (editor) » du hub Demos :
- **Game DB** : sélectionner un form → bouton Duplicate (la copie est
  sélectionnée, suffixe « Copy ») ; section « Used by » dépliable →
  clic navigue vers le référenceur.
- **Quests** : la quête « La menace de l'est » (chantier 6) s'affiche
  en arbre States→Branches→Tasks ; + State/+ Branch/+ Task créent avec
  le parent pré-rempli ; warnings inline (startState, destination,
  event) ; le nœud cliqué s'édite dans la grille en dessous.
- **Dialogues** : l'arbre du dialogue Villager ; + reply alterne les
  speakers ; + condition accroche un ConditionForm ; ^ réordonne.
- **Schedules** : une bande 24 h par schedule, une ligne par entrée ;
  tirer les bords (snap 0.5 h, un seul undo par drag) ; la ligne jaune
  = l'heure du slider, l'éval H7 au-dessus de chaque bande.
- **Plugins** : les conflits listent chaque écrivain AVEC sa valeur ;
  choisir un gagnant → « Generate synthesis patch » → data/
  synthesis.toml chargé en dernier (l'entête `# ... from: ...` = la
  provenance) ; recharger et vérifier la valeur arbitrée.
- L'export plugin (Game DB) porte toutes les éditions des cinq
  panneaux — c'est LA même session.

## Vérification

Par brique : build Debug (+Release si shader/perf) + suite headless +
smoke-run + commit. Les opérations de données (clone, scan used-by,
valeurs de conflit, génération de patch) sont doctestées headless
(EditSessionTest, ResolverTest, SynthesisTest) ; les panneaux ImGui se
valident à la main dans l'EditorScene.

## Garde-fous

- Un éditeur n'écrit JAMAIS ailleurs que dans l'EditSession ; la sortie
  est toujours un plugin (§5, « one more layer »).
- Nouveaux champs de Forms : APPEND uniquement ; ici seul FieldConflict
  (pas un Form) change de shape.
- Purge `*.dir` + rebuild complet si un type PARTAGÉ change de layout
  (leçon re-payée au chantier 7 avec TerrainParams).

---

# Briques 8.6-8.11 — Interfaces visuelles & présentation nodale

> Plan écrit 2026-07-09 (go dev, arbitrages actés le même jour). Objectif :
> donner une interface VISUELLE à tout ce qui n'en a pas — graphes nodaux
> (anim, quêtes, dialogues), timeline de clips, FX avec preview live,
> builder de conditions, éditeurs Effect/Ability. Décisions dev :
> canvas nodal = **imgui-node-editor** (thedmd) via CPM, pattern ImGuizmo
> (DOWNLOAD_ONLY + lib statique) ; positions x/y des nœuds = **fichier
> annexe éditeur** (`data/editor-layouts.toml`), JAMAIS des champs de
> Forms — les plugins restent propres. Invariant §5 inchangé : chaque
> éditeur écrit UNIQUEMENT dans l'EditSession, la sortie est un plugin
> ordinaire.

## Contexte (8.6+)

- Tous les panneaux 8.1-8.5 sont des méthodes `draw*()` d'UNE classe,
  `game/scenes/EditorScene.{hpp,cpp}` (~990 lignes). À partir de 8.6, les
  nouveaux éditeurs sont des CLASSES à part dans `game/ui/` (le précédent
  `ConsolePanel` : construit dans `reload()`, référence l'EditSession,
  `draw()` appelé depuis `EditorScene::drawUi`). EditorScene ne grossit
  plus — il orchestre. Les méthodes 8.2-8.4 existantes ne migrent pas
  (pas de refactor gratuit en plein chantier).
- Le précédent de manipulation directe à copier : `drawSchedules`
  (EditorScene.cpp) — preview live pendant le drag, UN SEUL `setField`
  au relâchement (un undo par drag).
- Les graphes existent déjà côté données, il ne manque que la vue :
  - **AnimGraph** (`data/forms/AnimForms.hpp`) :
    `AnimGraphForm{initialState}` → `AnimStateForm{parent, clip, speed,
    referenceSpeed}` → `AnimTransitionForm{parent, from, to, param,
    compare, threshold, …}` (`from == 0` = « any state »). AUCUN éditeur
    aujourd'hui — vrai graphe orienté, le premier consommateur naturel.
  - **Quêtes** (`quest/Quest.hpp`) : states = nœuds, `QuestBranchForm` =
    l'arête (`state` → `destination`), tasks = contenu de l'arête.
  - **Dialogues** (`quest/Dialogue.hpp`) : arbre par `parent`/`order`.
  - **Conditions** (`gameplay/condition/Condition.hpp`) : clauses ANDées
    accrochées par `parent` (nœud de dialogue, ability, état de quête).
- `fx::ParticleSim` (`engine/fx/Particles.{hpp,cpp}`) tourne headless et
  déterministe par seed (preuve : `tests/CuesSchedulesTest.cpp`) ;
  `forEach(position, size, color)` donne tout ce qu'il faut pour dessiner
  la preview en `ImDrawList` — pas besoin de render-to-texture.
- Contrainte du modèle plugin à assumer dans l'UI : un plugin ne SUPPRIME
  pas de record (§5). « Supprimer » un nœud/une arête n'est possible que
  pour les forms créés DANS la session (retrait du draft) — les records
  base s'affichent non-supprimables. D'où `EditSession::removeCreated`
  (8.6).

## Les briques (par valeur)

### 8.6 — Fondation node-canvas + éditeur AnimGraph

> **FAITE (2026-07-09, `496af72`) — validation dev en attente** (liste
> « quoi tester » ci-dessous). 336 tests verts (+10), smoke-run OK.
> Compat imgui-node-editor × imgui 1.92.8 : develop @ b302971 + 9 hunks
> (opérateur float*ImVec2 non gardé, `ImRect::Floor()` disparu).
> **Post-brique : la lib est VENDORÉE (`extern/imgui-node-editor/`)** —
> la route CPM+PATCHES cassait le clean rebuild du dev (le step de patch
> re-court sur la source partagée CPM_SOURCE_CACHE déjà patchée) ; le
> diff vs upstream reste documenté dans `upstream-imgui192.patch` +
> README du répertoire.

La fondation et son PREMIER consommateur dans la même brique — pas
d'échafaudage spéculatif : chaque helper n'existe que parce que
l'AnimGraph l'exige.

**a) Dépendance imgui-node-editor** (racine `CMakeLists.txt`, miroir
exact du bloc ImGuizmo) : CPM DOWNLOAD_ONLY, commit master épinglé
(la release v0.9.3 de 2023 vise imgui 1.89 — trop vieille pour notre
1.92.8), lib statique compilant `imgui_node_editor.cpp`,
`imgui_node_editor_api.cpp`, `imgui_canvas.cpp`, `crude_json.cpp`,
`IMGUI_DEFINE_MATH_OPERATORS`, lien `imgui` ; ajoutée à la liste de
`true-adventurer` (`game/CMakeLists.txt`, à côté d'`imguizmo`).
**Compiler la lib SEULE avant toute ligne d'UI** — c'est le risque n°1.
Config runtime : `ed::Config::SettingsFile = nullptr` (le json interne
désactivé — notre side-store possède les positions).
**Fallbacks dans l'ordre** : 1) bouger le commit épinglé ; 2) patch
minime via l'option `PATCHES` de CPMAddPackage (CPM 0.42.3 la supporte) ;
3) vendorer sous `extern/` (précédent glad). On ne bouge JAMAIS le pin
d'imgui — c'est la lib nodale qui s'adapte.

**b) Côté données (meadows-data, testable headless) :**
- `data/editor/GraphLayout.{hpp,cpp}` — auto-layout par couches, PUR
  (aucun ImGui) : entrée = nœuds (guids), arêtes, racines, clé d'ordre
  optionnelle ; BFS depuis les racines → x = profondeur × 280, y = rang
  × 110, tri intra-couche (clé d'ordre, guid) → DÉTERMINISTE ; cycles
  coupés, orphelins en couche finale.
- `data/editor/EditorLayouts.{hpp,cpp}` — le side-store :
  `positionOf(graph, node)` / `setPosition` / `load(path)` /
  `writeToml()` (écriture triée par guid — diffs git propres). Chargé
  dans `EditorScene::reload()`, sauvé au relâchement d'un drag de nœud.
- `EditSession::removeCreated(id)` — retire un draft CRÉÉ cette session
  (refus si record base), undoable. ⚠ EditSession est un type PARTAGÉ :
  purge `*.dir` + rebuild complet (leçon payée ×2).

**c) Côté UI (`game/ui/`, compilé dans `true-adventurer`) :**
- `game/ui/NodeCanvas.{hpp,cpp}` — wrapper mince et RÉUTILISABLE sur
  `ax::NodeEditor` : contexte ed (un par panneau), table guid↔id u64
  (compteur monotone par canvas, stable inter-frames), application des
  positions (side-store sinon auto-layout) au premier affichage,
  collecte des drags au relâchement → EditorLayouts, plomberie
  BeginCreate/BeginDelete remontée en ACTIONS (`newLink{from,to}`,
  `deleteRequest{guid}`, sélection). Le contenu des nœuds reste dessiné
  par chaque éditeur — pas de méga-abstraction, trois consommateurs en
  8.6-8.7 la calibrent.
- `game/ui/FormPicker.{hpp,cpp}` — combo « choisir un form de type T »
  (forEachVisible + filtre texte, affiche editorId). Réutilisé partout
  ensuite (8.9, 8.10, 8.11).
- `game/ui/AnimGraphPanel.{hpp,cpp}` — nœud = `AnimStateForm` (titre =
  editorId, lignes = clip résolu, speed), pseudo-nœud « Any State »
  (transitions `from == 0`), badge `initialState` + menu « Set as
  initial », lien = `AnimTransitionForm` (label `param compare
  threshold`, pointillé si `waitForEnd`), drag pin→pin =
  createForm+setField (parent/from/to), clic droit = « + State »
  (position au clic), sélection nœud/lien → `drawPropertyGrid` (MÊME
  session), suppression drafts créés uniquement. Warnings inline :
  initialState nul/dangling, transition sans `to`, état sans `clip`,
  `compare` inconnu.

- Headless : `tests/GraphLayoutTest.cpp` (déterminisme, diamant, cycle,
  orphelins), `tests/EditorLayoutsTest.cpp` (round-trip TOML),
  extension `EditSessionTest` (removeCreated + undo/redo + refus base).

**Quoi tester (dev)** : fenêtre « Anim Graph » de la scène « Game DB
(editor) » — créer un graphe + 3 états + transitions au drag ; l'état
initial badgé ; déplacer les nœuds, quitter/relancer → les positions
reviennent (`data/editor-layouts.toml`) ; « Auto-layout » range un
graphe jamais ouvert ; un lien cliqué s'édite dans la grille ; undo
défait la dernière transition ; export plugin → le TOML ne contient QUE
des records anim, aucun x/y.

### 8.7 — Graphes de quêtes et de dialogues

Deux consommateurs de plus sur le canvas tout chaud — la brique qui
VALIDE la généricité de NodeCanvas (si elle force des hacks, la
fondation se corrige ici, tôt).

- `game/ui/QuestGraphPanel.{hpp,cpp}` : nœuds = `QuestStateForm` (badge
  kind — Success vert, Failure rouge, marque sur le startState), arêtes
  = `QuestBranchForm` (label « n tasks », warning si `destination`
  nulle) ; drag pin→pin = create branch ; sélection d'arête →
  PropertyGrid du branch + liste de ses tasks avec « + Task » (le flux
  8.2). La fenêtre « Quests » (arbre 8.2) RESTE — le graphe est la vue
  structure, l'arbre la vue détail.
- `game/ui/DialogueGraphPanel.{hpp,cpp}` : l'arbre `parent`/`order` posé
  à plat — auto-layout avec `order` comme clé de rang ; nœud = speaker +
  extrait de texte (48 chars) + badge « n cond » ; « + reply » au menu
  contextuel (speaker alterné, order suivant — la logique 8.3) ;
  re-parentage par drag du lien = `setField(parent)` avec garde
  anti-cycle (refus si la cible est un descendant). Le
  ré-ordonnancement reste aux boutons `^` de l'arbre 8.3 (v1).
- Headless : anti-cycle doctesté ; layout d'arbre ordonné déterministe.

**Quoi tester (dev)** : « La menace de l'est » (chantier 6) lisible en
graphe — le start badgé, une branche sans destination en warning ; tirer
un lien crée un branch ; le dialogue Villager en graphe = mêmes données
que l'arbre 8.3 (les deux fenêtres montrent la MÊME sélection) ;
re-parenter une réponse au drag → l'arbre suit.

### 8.8 — Timeline des clips anim (events)

Le pont anim→gameplay (`AnimEventForm` : hit frames, footsteps, FX)
édité comme les schedules — ImDrawList, drag committé en UN setField au
relâchement.

- `game/ui/ClipTimelinePanel.{hpp,cpp}` : liste des `AnimClipForm` ;
  bande temporelle par clip : marqueurs = `AnimEventForm` (childrenOf,
  label = `name`), drag horizontal = retune de `time` (snap 0.01 s,
  commit au relâchement), clic sur un trou = « + event » (parent + time
  pré-remplis), sélection → PropertyGrid.
- ⚠ Assumé v1 : `AnimClipForm` ne porte PAS la durée (elle vit dans le
  glTF) → longueur de vue = max(times des events, 1 s) + champ « view
  length » ; la vraie durée d'asset = stretch.
- Navigation croisée : un état sélectionné dans l'AnimGraph ouvre son
  clip dans la timeline.

**Quoi tester (dev)** : le clip d'attaque : poser un event « Hit » à
0.35 s, le tirer, undo ; « + event » sur un trou ; l'export porte le
record ; ouvrir depuis le graphe (état → clip).

### 8.9 — Builder de conditions partagé

LE widget transverse — dialogues, quêtes, abilities parlent tous
`ConditionForm` (clauses ANDées par `parent`).

- `gameplay/condition/Condition.{hpp,cpp}` : ajouter
  `conditionSummary(const ConditionForm&)` — « if not HasTag
  Faction.Hostile »… Doctesté ; remplace les labels ad hoc de
  drawDialogues.
- `game/ui/ConditionBuilder.{hpp,cpp}` :
  `drawConditionList(session, parentId)` — une ligne par clause : combo
  `kind`, champs CONTEXTUELS au kind (tag / attribute+value / item via
  FormPicker / texte Lua), negate, résumé ; « + condition » (parent
  pré-rempli) ; suppression drafts uniquement.
- Intégrations : inspecteurs dialogue (graphe 8.7 + arbre 8.3), quêtes,
  et — préparé pour 8.11 — les abilities. Les transitions anim
  n'utilisent PAS ConditionForm (gates par tags dans le form).
- Headless : chaque kind produit via la séquence du widget une clause
  que `evaluateClause` évalue comme attendu (vrai/faux/negate).

**Quoi tester (dev)** : sur un nœud du dialogue Villager : ajouter
« HasTag », passer en « AttributeAtLeast health 50 », negate ; le résumé
suit ; le kind change → seuls les champs pertinents s'affichent ; la
clause exportée re-gate l'option en jeu après reload.

### 8.10 — Éditeur FX/Particules + preview live

- `gameplay/cue/GameplayCues.{hpp,cpp}` : ajouter
  `fx::EmitterParams toEmitterParams(const data::ParticleForm&)` — le
  mapping form→sim qui manquait (règle transverse : engine/fx ne voit
  jamais data::, le mapping vit côté gameplay). Doctesté champ à champ.
- `game/ui/FxPanel.{hpp,cpp}` : liste des `ParticleForm` ;
  PropertyGrid ; preview : un `fx::ParticleSim` membre + accumulateur
  qui émule `rate`/`duration`/`burst` (seed incrémentée —
  déterministe assez pour un outil) ; rendu =
  `ImDrawList::AddCircleFilled` par particule (projection ortho X/Y,
  ligne de sol — `forEach` fournit position/size/color interpolés) ;
  `blend == "additive"` approximé par sur-brillance (assumé : outil de
  FORME/timing, la couleur finale se juge en jeu) ; boutons Restart /
  seed / time-scale. Section « Cues » : les `CueForm` (tag,
  particles/sound via FormPicker) + « utilisé par » inversé
  (`data::referencesTo`).
- Headless : le mapping doctesté ; le déterminisme du sim est DÉJÀ
  couvert (CuesSchedulesTest).

**Quoi tester (dev)** : sélectionner un ParticleForm → la preview joue ;
monter `rate`, changer `colorStart` → visible au restart ; « additive »
plus lumineux ; créer un CueForm « Cue.Hit.Slash » pointant le particle,
exporter, vérifier en jeu (l'arène de combat émet ce cue).

### 8.11 — Éditeurs dédiés Effect & Ability

`EffectForm` et `AbilityForm` n'ont que la grille brute — l'éditeur
dédié apporte les enums en combos, le masquage contextuel et la
validation. PAS d'enfants à gérer (design Phase 3 : un modificateur par
effect, la composition = plusieurs effects).

- `game/ui/EffectPanel.{hpp,cpp}` : sections — Modifier (`attribute`,
  combo `op`, `magnitude`, attribute2/magnitude2 repliés) ; Duration
  (combo instant|duration|infinite|periodic, champs selon le kind) ;
  Tags (granted/required/blocked) ; Expiry (visible si attribute
  résonance) ; Buildup (combo des valeurs documentées). Warnings :
  periodic avec period ≤ 0, duration sans durée, buildup+attribute
  simultanés.
- `game/ui/AbilityPanel.{hpp,cpp}` : FormPickers filtrés EffectForm pour
  `cost`/`cooldown`/`effect` (navigation clic vers l'EffectPanel), combo
  `costPolicy`, tags, champ `script`, ConditionBuilder 8.9 (parent =
  ability). Warnings : cooldown sans `grantedTag`, guid dangling.
  **Sous-panneau « Test apply »** (la vraie valeur dev) : un
  AttributeSet/AbilitySystem jetables construits dans le panneau, bouton
  « Apply » → `applyEffect`/`tryActivate` headless, résultat affiché
  (attributs avant/après, tags, refus + raison). Zéro nouveau mécanisme.
- Headless : les règles de validation extraites en fonctions pures
  (`effectWarnings(const EffectForm&)` dans gameplay) et doctestées.

**Quoi tester (dev)** : ouvrir un effect existant → seuls les champs du
kind de durée s'affichent ; créer « poison » periodic → warning tant que
period = 0 ; une ability : brancher cost/cooldown/effect aux pickers,
« Test apply » montre l'énergie qui baisse et le tag cooldown posé ;
condition via le builder ; export → UN plugin, tous panneaux confondus
(LA même session, toujours).

### Stretch 8.6+ (si le chantier se plie vite)

- Minimap/`NavigateToContent` + zoom-to-selection sur les canvas.
- Durée réelle des clips (lecture glTF) pour la timeline 8.8.
- Shapes sphere/cone/box dans `fx::ParticleSim::spawnBurst` (le HOW TO
  FILL d'engine/fx) — la preview 8.10 les montre gratuitement.
- Duplication d'un graphe entier (clone récursif états+transitions).

## Vérification (8.6+)

Par brique : build Debug + suite headless complète (326 cas → ~340 :
GraphLayoutTest, EditorLayoutsTest, extensions
EditSession/Condition/FxMapping/effectWarnings) + smoke-run de la scène
« Game DB (editor) » + commit ; **validation dev ENTRE les briques**
(cadence standard — le run d'une traite de 8.1-8.5 était une exception
sur go explicite).

## Garde-fous (8.6+)

- Un éditeur n'écrit JAMAIS ailleurs que dans l'EditSession ; la sortie
  est toujours un plugin (§5). Le side-store `data/editor-layouts.toml`
  est la SEULE exception assumée : ce n'est PAS une donnée de jeu
  (jamais chargé par le runtime, jamais dans plugins.toml) — c'est de
  l'état d'outil, comme imgui.ini.
- AUCUN champ de Form ne change dans tout le plan (les positions ne
  polluent pas les plugins — la décision fondatrice). Seule API partagée
  modifiée : `EditSession::removeCreated` → purge `*.dir` + rebuild
  complet.
- Un plugin ne supprime pas de record : l'UI n'offre « delete » QUE sur
  les drafts créés en session ; un record base est immuable-visible.
- imgui-node-editor : commit ÉPINGLÉ (jamais de branche flottante) ;
  toute incompatibilité 1.92 se règle côté node-editor (PATCHES CPM ou
  vendoring extern/), JAMAIS en bougeant imgui. Abandon (improbable) =
  canvas maison ImDrawList façon drawSchedules.
