# Architecture : Simulation & Présentation

> Document de référence pour le projet. Décrit l'architecture cible du jeu
> (simulateur de vie d'aventurier moddable, horizon long terme) et les
> invariants à respecter pour toute décision technique. Quand un choix
> d'implémentation est ambigu, trancher en faveur de l'invariant.

## Principe fondateur

**L'actif durable du projet est la couche de simulation, pas le moteur.**
Le modèle de monde, les règles de jeu, le schéma de données et le système de
modding doivent survivre à tout changement de moteur de rendu. Le moteur
(Godot aujourd'hui, runtime custom plus tard) est un **frontend
interchangeable**, traité comme un loyer, jamais comme un mariage.

Corollaire : le cœur de simulation est écrit en **C++ pur, engine-agnostic**,
compilable et testable **sans Godot**. Aucune dépendance au moteur ne remonte
dans la logique de gameplay, la sérialisation ou le data-model.

## Invariant central (à ne jamais violer)

1. **Le Model ne connaît jamais la View.** La simulation ignore l'existence
   de Godot.
2. **La View est reconstruite depuis le Model**, à chaque tick, par projection.
3. **Toute mutation du Model passe par des messages explicites** : `intentions`
   (gameplay runtime) ou `commands` (édition). La View n'écrit jamais
   directement dans le Model.

Nom précis du pattern : *simulation autoritaire + vue matérialisée par
projection unidirectionnelle*, soit un **functional core / imperative shell**.
(Proche de MVVM mais sans data-binding bidirectionnel — le flux est
strictement unidirectionnel.)

## Découpage des responsabilités

| Couche | Rôle | Technologie |
|---|---|---|
| **Model** | État de vérité, logique, comportement, toute la simulation | C++ pur + **flecs (ECS)** |
| **Pont (ViewModel)** | Traduction état→vue, état de présentation (interpolation, culling), mapping `entity_id ↔ node`. Aucune logique de gameplay. | GDExtension / glue |
| **View** | Rendu + widgets d'édition. Projection visuelle uniquement. | Godot `SceneTree` |

- **flecs reste le cœur de simulation.** Godot ne le remplace en rien. Les
  entités de gameplay vivent dans flecs ; les nœuds Godot ne sont que leur
  projection visuelle.
- **Conserver un runner headless** (simulation sans rendu) : indispensable pour
  tester l'équilibrage du combat et le temps de jeu en lançant des milliers de
  parties en accéléré. C'est aussi le garde-fou qui prouve l'agnosticisme.

## Flux par tick

```
Model (flecs)  ──tick(delta)──►  résolution  ──projection──►  View (set_state)
     ▲                                                              │
     └──────────────────  intentions (push_intent)  ◄──────────────┘
```

- Godot pousse le `delta` (`_physics_process`) → `world.tick(delta)`.
- La sim résout tout le comportement, produit le nouvel état.
- Le pont synchronise les coquilles de vue depuis cet état.
- Les inputs joueur → intentions brutes → la sim décide.

## Vue & culling

- Une coquille générique `EntityView` (`Node2D`/`Sprite2D`) **sans logique**,
  configurée par `set_state(data)`. **Pas** une scène + un GDScript par type de
  monstre.
- **N'instancier des nœuds que pour les entités visibles.** flecs simule des
  milliers d'entités (PNJ vivant leur vie au loin) ; le pont crée/détruit les
  coquilles selon le champ visible (culling). Condition d'un monde vivant à
  grande échelle.
- État légitimement côté Godot : interpolation entre ticks, particules
  décoratives, screen shake, offsets d'animation — tout polish d'affichage
  **jetable**. Règle de tri : *si ça doit survivre à un changement de moteur,
  c'est dans flecs ; sinon, dans la coquille Godot.*

## Le contenu n'est que des données

**Le jeu de base est lui-même un mod** (le premier, sans privilège
structurel). La frontière donnée/code est la décision de design la plus
importante :

- **Le code définit les règles du jeu de règles ; les données peuplent le jeu.**
- Pousser un maximum vers la donnée : stats, attributs, objets, sorts, loot,
  courbes, résistances, dialogues, quêtes.
- Le **moteur de résolution** (calcul des stats, application des overrides,
  résolution du load order) reste du code.
- **Stat system** (Santé/Énergie/Essence + 9 attributs) = un interpréteur qui
  lit des définitions en données, **pas** du code en dur. Ajouter un attribut
  doit se faire en donnée.

### Comportement = donnée (jamais GDScript)

Pour que les mods ajoutent des *systèmes* et pas seulement du *contenu*, le
comportement doit être exprimable en données. Par ordre de préférence :

1. **Déclaratif** (machines à états, behavior trees, conditions→actions en
   données) — interprété en C++. Le plus aligné. À privilégier.
2. **Lua embarqué** dans la lib C++ si du scripting impératif est nécessaire.
3. **Plugins C++ natifs** (déjà en place) pour la logique compilée.

> **Interdiction stricte : GDScript ne porte aucun comportement de gameplay
> moddable.** L'utiliser couplerait le modding à Godot et tuerait
> l'indépendance moteur. GDScript reste cantonné au collage d'UI / transitions
> d'écran — présentation jetable uniquement.

Test de décision : *« si je remplace Godot demain, ce comportement doit-il
survivre ? »* Oui → donnée/script dans la sim. Non → peut être GDScript.

### Identité & dépendances

- **IDs stables** par élément, survivant aux versions (équivalent FormID).
- **Load order additif** avec overrides, résolu côté C++.
- Espaces de noms par mod, références croisées, gestion des dépendances et des
  références cassées. *Un écosystème de modding est un système de gestion de
  dépendances déguisé.*

### Format de données

- Source de vérité : **texte lisible et éditable à la main** (JSON/TOML/DSL),
  moddable sans éditeur. Cache binaire au chargement pour la perf si besoin.
- **Schéma + validation dès le départ.** Des messages d'erreur clairs sur une
  donnée malformée sont une fonctionnalité de premier ordre, pas une finition.

## Choix de moteur (justification)

- **Godot** : renderer + hôte de l'éditeur. Chargement de ressources au runtime
  natif, formats texte moddables, open source (indépendance décennale,
  forkable), GDExtension garde le cœur C++ intact et maître du load order.
- **Unreal écarté** : le pipeline de cooking va à contre-courant du modding
  « dépose-un-fichier ». Le moddeur aurait besoin de l'éditeur + du projet pour
  cooker → barrière d'entrée haute, SDK de modding entièrement à charge,
  pression à coupler le cœur au moteur. Désaligné avec une stratégie où le
  modding produit le contenu.
- **Moteur custom** : différé. Le cœur (flecs + data-model) est déjà custom ;
  on n'emprunte un moteur que pour rendu/éditeur/outils. La frontière custom
  remonte vers le rendu **de façon incrémentale et opportuniste**, seulement
  quand un besoin précis le justifie.
- Réserve connue : open-world 3D à très grande échelle (streaming, LOD
  lointains) = terrain le moins mûr de Godot. Problème *tardif*, isolé dans la
  couche moteur, réévaluable sans toucher à la simulation.

## Édition

- **Éditeur in-game**, dans le processus du jeu, construit avec des nœuds UI
  runtime de Godot (`Control`, `SubViewport`, `CodeEdit`…). **Pas** un fork de
  l'application éditeur Godot (modèle Battlefield Portal = éditeur séparé →
  pas de hot-reload live possible).
- **Play mode et edit mode = la même scène, la même couche de vue.** Edit mode
  ajoute l'UI d'édition et gèle la sim. Relation Creation Kit ↔ Skyrim, dans un
  seul binaire.
- **La sauvegarde passe par la couche C++**, qui écrit ton format
  (JSON/TOML/DSL) et les fichiers `.lua`. **Jamais** la sérialisation Godot
  (`.tres`/`.tscn`) — sinon recouplage de la source de vérité au moteur. Godot
  ne fournit que les widgets.
- **Undo/redo en command pattern côté C++**, là où vivent les données.

## Hot-reload (workflow cible)

Sélectionner une entité en runtime → changer ses refs graphiques / son script →
sauver → retester en place. Rendu quasi-gratuit par l'**indirection via IDs
stables** : une entité référence sa définition/son script via un registre ;
recharger = invalider l'entrée, relire, les entités prennent la nouvelle version
au tick suivant.

- **Définition de données** : la façade relit la définition sauvée et la
  ré-applique (décider : édition de l'archétype vs de l'instance).
- **Graphismes** : pour un asset moddé déposé au runtime, **bypasser le
  pipeline d'import Godot** — `Image.load()` + `ImageTexture.create_from_image()`
  au lieu de `load("res://…")`.
- **Lua** : garder les scripts de comportement **stateless** (fonctions
  recevant l'état de l'entité en paramètre) → reload = simple swap de fonction.
  De l'état interne imposerait une stratégie de migration.

## Checklist de décision rapide

- Ce comportement doit-il survivre à un changement de moteur ? → sim (flecs/
  données/Lua), pas GDScript.
- Cette donnée est-elle du contenu de jeu ? → format texte moddable, pas en dur.
- Cette mécanique peut-elle être exprimée en données ? → si oui, la pousser
  hors du code.
- Cette mutation touche-t-elle le Model ? → via intention (runtime) ou command
  (édition), jamais d'écriture directe depuis la View.
- Cette sauvegarde écrit-elle l'état de vérité ? → via la couche C++, pas via
  Godot.
