# Chantier 4 — Interfaces : « les écrans du jeu »

> **PLANIFIÉ (2026-07-06)** — exécution au prochain « go » du dev (même
> mode d'une traite possible que les chantiers 2-3 si le dev le demande).
> État global : `docs/MEADOWS-PLAN.md`. Contrat des seams :
> `docs/HORIZONTAL-PASS.md` § UI (H4) — suivre, ne pas redessiner.

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
