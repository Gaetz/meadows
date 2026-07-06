# Chantier 4 — Interfaces : « les écrans du jeu »

> **FAIT (2026-07-06) — exécuté d'une traite sur « go » du dev** (même
> mode que les chantiers 2-3). 258 tests / 77 921 assertions verts.
> Validation visuelle dev EN ATTENTE (liste « quoi tester » dans le
> rapport de fin de session). Journal d'exécution et pièges payés en fin
> de fichier. État global : `docs/MEADOWS-PLAN.md`.

## Contexte

Quatrième chantier : les écrans RmlUi P0 (catalogue G de MEADOWS-PLAN) +
le reliquat outils J (console, plugin manager) + le **barter reporté du
chantier 3**. Décision actée : UI de jeu = **RmlUi**, documents `.rml/
.rcss` servis par les `ui/` des plugins avec overlay par chemin (un mod
override un écran — le modèle SkyUI). Les outils dev restent ImGui.

C'est aussi le chantier qui résorbe l'**écart connu n°1** de
HORIZONTAL-PASS : basculer les scènes sur `loadPluginStack`
(plugins.toml) au lieu des chargements en dur.

## Acquis (ne pas recréer)

- **Seam RmlUi (H4)** : `engine/ui/UiSystem` (lib meadows-ui) — rendu sur
  RHI (scissor + alpha prémultiplié), FileInterface = overlay par chemin
  sur une liste de roots (dernier gagnant), souris branchée
  (`processMouseMove/Button/Wheel`), `showDocument(path)`. La scène
  UiDemoScene prouve le rendu ET l'override par mod. **Manque** (note
  « comment remplir » en tête du header) : pile d'écrans, clavier/texte,
  data binding, localisation.
- **`UiScreenForm`** (H1) : registre des écrans — `screen` (nom logique),
  `document` (chemin ui/), `modal`, `overlay`. Réfléchi, doctesté.
- **`loadPluginStack` + plugins.toml** (H1) : utilisé par l'EditorScene
  seulement — LandscapeScene charge landscape.toml en dur (écart n°1).
- **Gameplay à brancher, tout existe** : stats/HUD (StatBlock, DerivedStats
  Phase 6), inventaire + équipement (Phases 3/7 : ItemForm, poids, valeur,
  equip par slot), dialogues (arbres Phase 4 + condition evaluator, records
  décomposés), repos (`gameplay::sleep()`, branché chantier 3),
  interaction E (prompt ImGui du chantier 3 → à migrer dans le HUD).
- **Outils H2** : GameDB browser, PluginsPanel (conflits par champ),
  console dev (get/set réflexion + REPL Lua) — squelettes LIVRÉS ; il
  manque des commandes de jeu et l'affichage des dépendances.
- **Police** : `game/data/base/ui/fonts/DemoFont.ttf` (placeholder — le
  dev dépose une vraie police quand il veut, workflow kit).

## Référence de design (directive dev 2026-07-06) : SkyUI / Norden UI

Le dev veut des inventaires **en tableau**, à la SkyUI (le standard de
facto du modding Skyrim) avec la direction visuelle de Norden UI /
NORDIC UI (reskins modernes de SkyUI). Ce qu'on retient :

- **Table multi-colonnes triable** : Nom, Poids, Valeur (+ Dégâts ou
  Armure selon la catégorie) ; clic sur l'en-tête = tri asc/desc.
- **Onglets de catégories** en barre du haut : Tout / Armes / Armures /
  Consommables / Livres / Divers — filtre instantané.
- **Champ de recherche** à correspondance partielle sur le nom.
- **Icônes d'état** en colonne : équipé, compteur (xN) ; (volé/enchanté
  plus tard, quand ces systèmes existeront).
- **Panneau de détail** de l'item sélectionné (nom, description, stats) ;
  la preview 3D de l'objet = P2 (demande du render-to-texture).
- **État persistant** : catégorie/tri/scroll restaurés à la réouverture.
- **Direction visuelle Norden/NORDIC** : moderne épuré, palette sobre
  quasi monochrome, panneaux translucides, typographie nette — pas de
  parchemin ; en phase avec la DA stylisée BotW.
- Le **barter et les conteneurs réutilisent le même tableau** (c'est
  exactement le modèle SkyUI) — B3/B5 partagent le composant.

RmlUi couvre ça nativement (flexbox + tables RCSS, data binding pour les
lignes) ; le tri/filtre vit dans le C++ (ScreenStack/binding), le .rml ne
fait que l'affichage — un mod reskinne sans toucher à la logique.

## Les briques proposées

### B1 — Socle : plugin stack + pile d'écrans + clavier
- **Bascule `loadPluginStack`** : LandscapeScene (et les scènes de démo
  qui restent) chargent via plugins.toml ; unifier l'enregistrement des
  types (les 2 sites complets : EditorScene::onEnter et cooker Main → une
  fonction partagée `registerAllFormTypes`). landscape.toml + village.toml
  + level-edits.toml deviennent des entrées du stack. Doctest : le stack
  résout identique au chargement en dur (non-régression).
- **UiSystem généralisé** : documentRoots = les `ui/` de CHAQUE plugin du
  stack, dans l'ordre (PluginStack.baseDir — le UiDemoScene n'en branche
  qu'un).
- **Pile d'écrans** (`game/ui/ScreenStack` ou world/runtime — logique PURE,
  doctestable headless) : `UiScreenForm` registry → `showScreen(name)` /
  `closeTop()` ; modal = capture souris + pause du sim en dessous ;
  overlay (HUD) toujours rendu sous la pile. Échap = pause/fermer le top.
- **Clavier** : compléter `processKey/processText` dans UiSystem (les
  types Rml restent dans le .cpp) ; routage depuis platform::Input quand
  un modal est ouvert.
- Preuve : Échap ouvre/ferme un écran vide, la souris se libère, le sim
  se fige.

### B2 — HUD + data binding
- **Façade DataModel** dans UiSystem (aucun type Rml en header) : bind de
  valeurs nommées (f32/str/bool + tableaux simples) vers les documents.
- `hud.rml` (overlay) : barres santé/énergie/essence + posture, crosshair,
  **prompt d'interaction** (migre le prompt ImGui du chantier 3), heure du
  jour. Boussole simple (cap caméra) si trivial, sinon noter P1.
- Preuve : les coups du bandit font baisser la barre ; le prompt [E]
  s'affiche en RmlUi.

### B3 — Inventaire + conteneur/loot (modèle SkyUI, voir § Référence)
- `inventory.rml` (modal, touche I ou Tab) : **table triable** (Nom,
  Poids, Valeur, Dégâts/Armure contextuels), onglets de catégories,
  recherche, icône « équipé » + compteur, panneau de détail de l'item
  sélectionné ; total porté / CarryWeight en pied. Actions :
  équiper/déséquiper (EquipmentStats Phase 7), utiliser (consommables →
  applyEffect), jeter. Tri/filtre/état = C++ (logique pure doctestable :
  `InventoryView` — tri par colonne, filtre catégorie, recherche) ; le
  .rml n'affiche que les lignes bindées.
- **Conteneur/loot** = le même tableau en mode transfert (deux panneaux) :
  E sur un conteneur ou le cadavre du bandit → prendre/déposer.
- Doctests : les opérations inventaire/équipement sont déjà testées —
  ajouter `InventoryView` (tri/filtre/recherche) et le round-trip
  transfert conteneur si manquant.
- Preuve : looter le bandit, trier par valeur, chercher « ration »,
  équiper l'épée rouillée, manger une ration.

### B4 — Dialogue
- `dialogue.rml` (modal) : texte du PNJ + choix du joueur — branché sur
  les arbres de dialogue Phase 4 (records + condition evaluator), remplace
  le placeholder « Parler » du chantier 3.
- Contenu : un petit dialogue pour le Villager dans village.toml (2-3
  nœuds, une condition pour prouver l'evaluator, une sortie).
- Preuve : conversation complète au clavier/souris.

### B5 — Barter (reliquat chantier 3)
- **L'or est un item** (`ItemForm` GoldCoin, count) — pas de nouveau champ
  ni de monnaie parallèle ; réutilise l'inventaire tel quel.
- `barter.rml` (modal) : **le même tableau que B3** (composant partagé,
  modèle SkyUI) en deux panneaux joueur/marchand, colonne Prix = valeur ×
  multiplicateurs achat/vente (constantes StatsTuningForm — moddable),
  or affiché des deux côtés.
- Marchand v1 : le Villager (ou un 2e PNJ marchand ActorForm) avec un
  inventaire de départ posé en dur dans village.toml ; l'option
  « Commercer » s'ouvre depuis le dialogue (B4). `VendorForm`/restock =
  P1 économie, ne pas l'anticiper.
- Doctest : transaction headless (prix, transfert, or) — logique pure.
- Preuve : acheter/vendre une ration au village.

### B6 — Menus : principal, pause, repos/attente
- `main-menu.rml` : Nouvelle partie / Quitter (Charger = chantier 5,
  bouton grisé). `pause.rml` (Échap) : Reprendre / Menu principal /
  Quitter.
- **Menu d'attente** (touche T) : choisir 1-24 h → `gameplay::sleep()`
  (le repos du chantier 3 gagne son UI ; le lit continue d'ouvrir le
  sommeil directement).
- **Hook workstation** : `FurnitureForm.screen` → `showScreen(...)` (le
  champ déclaré à H1 devient réel ; l'écran de craft lui-même = P1 — un
  placeholder « atelier » suffit à prouver le câblage).
- Preuve : boucle complète menu → jeu → pause → menu.

### B7 — UI monde + commandes console
- **Nameplates/barres de vie ennemis** (P0 « UI monde ») : projection
  écran des personnages hostiles/blessés proches → éléments HUD via le
  data binding (pas de nouveau système de rendu).
- **Console** (squelette H2) : commandes de jeu maintenant que les
  systèmes existent — `spawn <editorId>`, `tp <x> <z>`, `tgm`,
  `settime <h>`. Réutiliser le Spawner/GameClock, aucune mécanique neuve.
- **PluginsPanel** : afficher les dépendances déclarées. L'outil de
  synthesis §5.1 (arbitrage par champ → plugin ordinaire) reste NOTÉ mais
  ne se construit que si le besoin mord — prérequis rappelé :
  `FieldConflict` doit porter les valeurs des écrivains.
- Preuve : `spawn Bandit` + nameplate au-dessus, barre qui baisse.

### B8 — Clôture
- CHANTIER-4 annoté (journal + pièges), MEADOWS-PLAN (coches G/J + table
  d'état, chantier 5 = PROCHAIN), HORIZONTAL-PASS (§ UI rempli, écart n°1
  clos), userdoc (`ui-modding.md` : écrans réels + override par mod ;
  nouvelle page ou section « jouer » si utile), mémoire agent.

## Fichiers principaux

- Nouveaux : `game/ui/ScreenStack.{hpp,cpp}` (logique pure + doctests),
  documents `game/data/base/ui/*.rml|rcss` (hud, inventory, dialogue,
  barter, main-menu, pause, wait), records `UiScreenForm` dans base/
  village toml, dialogue Villager + GoldCoin + inventaire marchand
  (village.toml).
- Étendus : `UiSystem` (processKey/processText, DataModel façade, roots
  multi-plugins), `LandscapeScene` (loadPluginStack, intégration pile +
  HUD, migration du prompt), `ConsolePanel` (commandes), PluginsPanel
  (dépendances), StatsTuningForm (multiplicateurs barter, APPEND).

## Réalisé — journal d'exécution (2026-07-06)

| Brique | État | Notes |
|---|---|---|
| B1 socle | ✅ | `data/plugins.toml` pilote jeu ET éditeur (écart n°1 HORIZONTAL-PASS clos ; fichiers absents = `skipped`, pas une erreur) ; `game/AllForms` = site d'enregistrement unique de l'exécutable (le cooker garde sa liste : il ne linke pas game/) ; `ScreenStack` pur doctesté ; UiSystem : documents par chemin, clavier/texte, façade DataModel (scalaires + rows + événements — aucun type Rml en header) ; canal d'Input événementiel (répétition OS, molette, texte UTF-8) ; **Échap ne quitte plus l'appli** (quit = bouton du menu / croix) |
| B2 HUD | ✅ | hud.rml overlay : barres santé/énergie/essence/posture, crosshair, horloge, prompt [E] + réplique migrés d'ImGui ; thème Norden (theme.rcss) partagé par tous les écrans |
| B3 inventaire | ✅ | `InventoryView` pur doctesté (onglets, recherche, tri par colonne avec bascule asc/desc, sélection persistante) ; table SkyUI + panneau détail (équiper/déséquiper/utiliser) ; conteneur = même table en deux panneaux (clic = transfert, Take all) ; **le cadavre du bandit se fouille** ; l'épée du joueur est vraiment dans son sac (équipée) et le swing lit l'ÉQUIPEMENT ; `MiscItemForm` (l'or), `goldValue` APPEND sur Armor/Consumable, `restoreHunger/Thirst` (manger) ; mods d'équipement pliés dans le tick |
| B4 dialogue | ✅ | DialogueRunner Phase 4 branché : dialogue du Villager (option gated HasItem rations — l'evaluator visible) ; clauses Lua fail-closed (VM non câblée en scène, note) |
| B5 barter | ✅ | Or = item ordinaire ; barterBuy/Sell purs doctestés (richesse du marchand finie) ; prix = goldValue × StatsTuningForm.barterBuy/SellMult (APPEND) ; **LoadoutEntryForm** (§C.1) rolls au spawn (stock marchand, poches du bandit, bourse joueur) remplace tout loot codé en dur ; « Voyons vos marchandises » → événement OpenBarter |
| B6 menus | ✅ | Menu principal au boot (Enter the world/Quit ; Load grisé = chantier 5), pause (Échap en Play), attente (T : 1/4/8 h — horloge + besoins, sans récup de lit), `Engine::requestQuit` ; hook `FurnitureForm.screen` réel (Workbench sur la place → workshop.rml placeholder) |
| B7 UI monde + console | ✅ | Nameplates (hostiles/blessés seulement) via projection écran → modèle HUD ; console F8 en jeu (reflection get/set + Lua + spawn/tp/tgm/settime, spawns transients) ; PluginsPanel : dépendances ok/après/manquante |
| B8 clôture | ✅ | Ce fichier, MEADOWS-PLAN, HORIZONTAL-PASS, userdoc, mémoire |

**Écrans livrés** (`UiScreenForm` × 9, documents dans `base/ui/`) : hud
(overlay), inventory, container, dialogue, barter, pause, mainmenu, wait,
workshop — tous préchargés au démarrage (une erreur de document moddé se
voit dans le log immédiatement).

**Pièges payés** :
- **`data-model` JAMAIS sur `<body>`** : les attributs du body
  s'appliquent après le parse des enfants → les vues STRUCTURELLES
  (data-for) sont silencieusement sautées et les enfants se lient comme
  des vues plates qui échouent. Toujours un div englobant.
- **`data-model` imbriqué interdit** (un re-bind dans un sous-arbre lié) ;
  faire transiter la valeur par le modèle du sous-arbre.
- Les modèles de données Rml **gèlent leurs slots à la création** — tout
  nom référencé par un document doit être déclaré avant son chargement
  (d'où le préchargement des écrans : les erreurs se voient au boot).
- **Objets stales × 2** : changer le layout de `platform::Input` puis de
  `StatsTuningForm` a produit deux crashs au démarrage (tas corrompu,
  mort avant le premier log utile). Après TOUT changement de layout d'un
  type partagé : purge des `*.dir` de NOS cibles + rebuild (leçon Phase 5,
  payée deux fois de plus). Le log flushe désormais chaque ligne (le
  crash n'avale plus la fin du log).
- Le zombie `cl.exe` (C1041 vc143.pdb) revient si on lance DEUX builds en
  parallèle (un en arrière-plan oublié) — un seul build à la fois.
- ~~`row.cells[i]`~~ : les lignes de table passent par des champs nommés
  plats (`c0..c4`) — l'indexation de tableaux dans les expressions Rml
  n'est pas fiable ; les cellules nommées se lient trivialement.

## Garde-fous

- **Aucun type Rml hors des .cpp de meadows-ui** (règle pimpl n°3) ; UN
  SEUL UiSystem par process (piège payé H4) ; couleurs prémultipliées,
  géométrie compilée = buffers retenus.
- **Les écrans sont des documents moddables** : jamais de layout en dur
  côté C++ — le C++ fournit les données (binding) et la pile ; le .rml
  décide de l'apparence. Test d'override par mod = déjà prouvé, ne pas le
  casser.
- La logique (pile, transactions barter, transferts) se doctest
  **headless** — l'UI n'est que la vue (règle n°5).
- Champs nouveaux sur Forms existants = **APPEND uniquement** (ordinaux
  binaires stables).
- Pas de gamepad (P1), pas de localisation systématique (P1 — mais
  UTF-8 partout), pas d'éditeur d'UI (P1), pas de carte (P1).
- Le sim ne sait rien des écrans : `gameplay/` n'inclut rien de
  meadows-ui ; la pile vit côté game/runtime (§2.10).
