# Chantier 8 — Outillage

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

## Vérification

Par brique : build Debug (+Release si shader/perf) + suite headless +
smoke-run + commit. Les opérations de données (clone, scan used-by,
valeurs de conflit, génération de patch) sont doctestées headless ; les
panneaux ImGui se valident à la main dans l'EditorScene (F10).

## Garde-fous

- Un éditeur n'écrit JAMAIS ailleurs que dans l'EditSession ; la sortie
  est toujours un plugin (§5, « one more layer »).
- Nouveaux champs de Forms : APPEND uniquement ; ici seul FieldConflict
  (pas un Form) change de shape.
- Purge `*.dir` + rebuild complet si un type PARTAGÉ change de layout
  (leçon re-payée au chantier 7 avec TerrainParams).
