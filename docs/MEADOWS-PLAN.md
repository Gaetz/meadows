# MEADOWS-PLAN — Fonctionnalités moteur & outils pour le Skyrim-like

## OÙ ON EN EST (mis à jour 2026-07-06)

| Piste | État |
|---|---|
| Décisions de cadrage (6) | ✅ actées 2026-07-05 (§ Décisions actées) |
| **Passe horizontale** (architecture de tous les systèmes) | ✅ FAITE 2026-07-06 — contrat : `docs/HORIZONTAL-PASS.md` (audit de compat inclus) |
| Renderer paysage (briques 27-31) | ⏸️ en pause — **absorbées par les chantiers** (27→1, lumières int.→2, 28-31→6) ; spec détaillée : `docs/3D-RENDERER.md` |
| **Chantier 1 — Socle 3D gameplay** (matériaux/skinning/anim/Jolt/caméra) | ⬅️ **PROCHAIN** (à planifier) |
| Chantier 2 — Monde habitable (cellules 3D, intérieurs, éditeur de niveau v1, prefabs) | à faire |
| Chantier 3 — Vivant (navmesh, IA/schedules exécutés, combat 3D, cues/FX/audio P0) | à faire |
| Chantier 4 — Interfaces (écrans RmlUi P0, GameDB/console déjà livrés en squelette) | à faire |
| Chantier 5 — Persistance (Phase 10 : streaming + save = couche de patches) | à faire |
| Chantier 6 — P1 par valeur (quêtes outillées, économie/crime, éditeurs anim/FX/UI…) | à faire |
| Refonte herbe (renderer) | ⏸️ en attente des recherches du dev |

> **CE FICHIER EST LA ROADMAP UNIQUE.** Les phases 0-8 du CLAUDE.md §9 sont
> de l'histoire (journaux `docs/PHASE-*.md`) ; les phases 8.5-14 ont été
> absorbées ici (mapping dans CLAUDE.md §9). Catalogue des chantiers
> nécessaires pour faire la **démo de gameplay dans Meadows** (décision
> 2026-07-05) plutôt que Godot/Unreal. Un passage sur un
> moteur externe reste possible plus tard — le seam sim/présentation (§2.10
> du CLAUDE.md) reste donc un invariant à protéger. Ce fichier liste QUOI ;
> chaque chantier fera l'objet de sa propre planification en briques le
> moment venu.
>
> Fil conducteur : **tout contenu est un Form moddable (§5), tout outil
> écrit des plugins**. L'éditeur de niveau interne n'est pas un système à
> part — c'est un auteur de plugins comme un autre (records `ReferenceForm`,
> résolution champ par champ, last-writer-wins). C'est l'avantage décisif
> du moteur maison sur Godot/Unreal pour CE jeu.

## Priorités

- **P0** — indispensable à une *vertical slice* Skyrim-like (un extérieur,
  un intérieur, un donjon ; combat 3D ; un PNJ à routine ; une quête).
- **P1** — nécessaire à la démo complète.
- **P2** — confort / plus tard / après la démo.

## Ce qu'on a déjà (acquis, ne pas refaire)

Réflexion + Forms + résolution de plugins champ par champ + cooker (Phase 1),
ECS/monde/cellules 2D (Phase 2), GAS complet + stats/équipement/statuts
(Phases 3-7), Lua + quêtes/dialogues/conditions (Phase 4), seam
multithreading + snapshot de rendu (Phase 5), combat 2D (Phase 8 en cours),
renderer 3D paysage complet avec météo/culling/compute (`docs/3D-RENDERER.md`),
loader glTF statique (cgltf), TomlWriter, RNG seedé, VFS d'assets par GUID
(design §5 — état d'implémentation à vérifier).

---

## A. Monde, niveaux & éditeur

> **Décision structurante (2026-07-05) : le monde est FAIT MAIN.** Les
> extérieurs sont auteurés à la main ; la génération procédurale n'existe
> que comme **outil d'assistance à l'authoring** (sortie = données
> retouchables), jamais comme système runtime du monde canonique.

| Fonctionnalité | Prio | Notes |
|---|---|---|
| **Streaming 3D worldspace→cells** (Phase 10 du CLAUDE.md) | P0 | Cellules extérieures async (le pattern chunks du renderer EST le prototype), intérieurs = worldspaces séparés, transitions portes + chargement, persistance par cellule. |
| **Éditeur de niveau interne au jeu** | P0 | Mode éditeur dans la scène : sélection/placement de références (gizmos translate/rotate/scale, snap, duplication), palette d'objets depuis la GameDB, **sculpt de terrain** (voir ci-dessous), **sortie = plugin TOML** (records ReferenceForm) via TomlWriter. Undo/redo (= inversion de patches). Caméra éditeur, picking (raycast). |
| **Système de prefabs** | P0 | `PrefabForm` = liste de références enfants relatives à un pivot (+ prefabs imbriqués). Placement en jeu = instanciation du groupe ; overrides par instance en patches champ par champ (§5 s'applique tel quel). L'éditeur sait « créer un prefab depuis la sélection ». |
| **Intérieurs** | P0 | Cellules intérieures : pas de terrain/ciel, kit de modules (murs/sols — tilesets 3D), éclairage local (voir B), occlusion simple par pièce (portals P2 ; v1 = cellule entière), ambiance audio/reverb dédiée. |
| **Terrain fait main** | P0 | Terrain extérieur = **heightmaps auteurées par worldspace** : sculpt dans l'éditeur (raise/lower/flatten/smooth) + peinture de splat, stockage en données moddables (patches de terrain par plugin), streaming par cellules inchangé. Routes/chemins (splines) et rivières/lacs placés P1. Le `TerrainNoise` actuel reste (a) le bac à sable du renderer paysage et (b) un outil « seed → terrain de départ » qu'on retouche — jamais la base du monde jeu. |
| **Outils procéduraux d'assistance** | P1 | **Brosses de scatter** dans l'éditeur (une brosse ÉCRIT des références dans le plugin — outil d'authoring, pas système runtime ; l'herbe cosmétique peut rester rule-based, elle ne porte pas de gameplay). **Générateur de bases de donjons par règles** (assemblage de kits de modules → sortie = records/prefabs retouchables à la main) P1/P2. |
| **Volumes de gameplay** | P0 | Triggers (déjà en 2D — port 3D), volumes d'eau, kill-z, zones de son/reverb, bornes de cellule. |
| **Marqueurs** | P0 | Spawn points, patrol points, heading markers ; le mobilier interactif est un système à part entière (voir F, `FurnitureForm`). Ce sont des références invisibles = déjà le modèle. |

## B. Rendu — compléments gameplay (au-delà du paysage)

| Fonctionnalité | Prio | Notes |
|---|---|---|
| **Matériaux & textures sur meshes** | P0 | Le renderer actuel est vertex-color. Il faut : sampling de textures albédo (+ normal P1) sur meshes statiques, `MaterialForm` (données, moddable), pipeline KTX2/Basis à activer, atlas/array pour l'instancing. Adapter au look stylisé (albédo plat + rampe). |
| **Lumières locales + ombres intérieures** | P0 | Point/spot lights en Forms placées comme références (`LightForm`), N lumières par objet (forward simple d'abord, clustered P1), ombres pour 1-2 lumières clés par cellule intérieure, flicker/candles (anim de params). Le paysage reste sun-only. |
| **Meshes skinnés (GPU skinning)** | P0 | Extension du loader glTF : squelettes, poids, upload des palettes de bones (UBO/SSBO — l'extension compute peut servir), rendu instancié des personnages. |
| **Transparence triée** | P1 | Alpha blend trié par distance (verre, fantômes, eau intérieure) ; le cutout reste l'exception (leçon fill-rate). |
| **Émissifs & enchantements** | P1 | Param émissif dans MaterialForm (bloom existant fait le reste), effets de matériau animés (dissolve, glow d'enchantement) pilotés par GameplayCues (voir F). |
| **Décals** | P1 | Sang, brûlures, impacts — projection simple boîte. |
| **LOD/impostors bâtiments & props placés** | P1 | Le système de variantes instanciées existe ; ajouter mesh LOD par distance et impostors P2. |
| **First/third person** | P0 | Caméra 3e personne avec collision (spring arm + raycast), 1re personne P1, caméras de dialogue (cadrage) P1. |

## C. Personnages & animation

| Fonctionnalité | Prio | Notes |
|---|---|---|
| **Système d'animation squelettale** | P0 | Clips glTF, échantillonnage, blending (cross-fade), couches (haut/bas du corps), masques par os, additive P1. **Décidé (2026-07-05) : pas de root motion** — contrôleur cinématique + anims in-place, playback rate calé sur la vélocité contre le foot-sliding. |
| **Contrôleur d'anim en données** | P0 | `AnimGraphForm` : états + transitions + conditions (réutiliser le **condition evaluator** existant !), paramètres pilotés par le gameplay (vitesse, InCombat via tags GAS). Moddable — un mod ajoute une animation d'attaque. |
| **Événements sur timeline** | P0 | Frames taguées dans les clips : hit frame (fenêtre de dégâts GAS), footsteps (audio par matériau), spawn FX, camera shake. C'est LE pont anim→gameplay. |
| **Éditeur d'animation / timeline** | P1 | Outil ImGui : preview du personnage, scrub, pose des événements, réglage des transitions/blend times, sauvegarde en Form. (P0 minimal : édition TOML à la main + hot-reload.) |
| **Apparence modulaire des personnages** | P0 | Slots visuels (tête/cheveux/torse/jambes/mains/pieds) = meshes skinnés interchangeables sur le même squelette ; teintes (peau/cheveux) ; `AppearanceForm` par PNJ. Le lien équipement→visuel : équiper une armure échange le mesh du slot (les données d'équipement existent depuis la Phase 7). |
| **Attach points** | P0 | Sockets sur os (main droite = arme, dos = fourreau, selle P2). Armes visibles rangées/dégainées. |
| **Regard & tête** (look-at IK) | P1 | Les PNJ regardent leur interlocuteur ; bouche qui bouge en dialogue P2 (subtitle-first). |
| **Anim LOD** | P1 | Fréquence d'update réduite avec la distance ; pas d'anim hors écran (le culling sait déjà qui est visible). |
| **Ragdoll** | P2 | Avec Jolt ; v1 = animations de mort. |
| **Character creator joueur** | P2 | L'apparence modulaire le rend possible ; UI dédiée plus tard. |

## D. Physique & contrôleur (Jolt)

| Fonctionnalité | Prio | Notes |
|---|---|---|
| **Intégration Jolt** | P0 | Collision statique des cellules (mesh colliders cuits par le cooker), broadphase, raycasts/shape casts (interaction, combat, caméra), triggers. Le monde physique vit côté sim (headless-testable avec Jolt headless). |
| **Character controller** | P0 | Capsule cinématique : pentes, marches, saut, chute (dégâts existants), nage P1, sprint (coûts GAS existants). |
| **Objets dynamiques** | P1 | Props poussables, loot qui tombe, physique légère (le « clutter » Skyrim). Havok-cheese P2. |
| **Portes & mécanismes** | P0 | Portes animées + verrous (crochetage = minigame P1), leviers, coffres — états persistés (= patches de référence, §5 natif). |

## E. IA & navigation

| Fonctionnalité | Prio | Notes |
|---|---|---|
| **Navmesh (Recast/Detour)** | P0 | Génération offline par cellule (cooker) + liens (portes, sauts), requêtes de chemin async (JobSystem), agents avec évitement local simple. |
| **Emploi du temps des PNJ** | P0 | `ScheduleForm` : entrées = plage horaire (game clock existant) × activité × lieu (marqueur/mobilier) × conditions (condition evaluator existant). Interruptions (combat, dialogue) avec reprise ; simulation dégradée hors cellule P2. La couche AU-DESSUS des packages : le schedule décide quoi/quand/où, le package exécute. |
| **Packages IA (exécution)** | P0 | `AiPackageForm` = les comportements exécutables (dormir/manger/travailler/errer/voyager/utiliser un mobilier), pilotés par le schedule ou empilés conditionnellement. |
| **Outil emploi du temps** | P0/P1 | P0 = **vue debug en jeu** : « où va ce PNJ, quel schedule/package le pilote, pourquoi » — inestimable pour débugger. P1 = éditeur visuel : timeline journalière par PNJ (blocs horaires drag & drop, lieu + activité), sortie = plugin comme tout le reste. |
| **IA de combat 3D** | P0 | Port du chase/attack 2D : distances d'engagement, strafe, usage d'abilities GAS (coûts/cooldowns existants), fuite (courage/santé), appel à l'aide (factions existantes). |
| **Perception 3D** | P0 | Vue (cône + raycast d'occlusion — la furtivité de la Phase 9 en dépend), ouïe (événements sonores gameplay), mémoire de dernière position connue. |
| **Foules/planification** | P2 | Budget d'update IA par frame, LOD IA (PNJ hors cellule simulés grossièrement — « offscreen simulation » Skyrim). |

## F. Gameplay (au-dessus du GAS existant)

| Fonctionnalité | Prio | Notes |
|---|---|---|
| **Combat 3D** | P0 | Traces d'armes (shape cast sur fenêtres de hit des anims), directions de blocage, projectiles (flèches/sorts — gravité simple), lock-on optionnel, staggers/posture déjà en données. Tout passe par les GameplayEffects existants. |
| **GameplayCues (pont effets→présentation)** | P0 | **Pièce architecturale clé** : mapping data-driven `tag d'effet → {FX, son, décal, shake, matériau}`. Le sim émet des cues (headless-safe, no-op sans frontend) ; le frontend les résout. C'est ce qui garde le GAS présentation-agnostique ET moddable (un mod ajoute un sort avec ses visuels sans C++). |
| **Interaction** | P0 | Raycast d'interaction + prompt contextuel (Prendre/Parler/Ouvrir/Dormir), activation via le système existant. |
| **Mobilier & postes de travail** | P0 | `FurnitureForm` — système PARTAGÉ joueur/PNJ : points d'usage orientés (slots d'occupation, file si occupé), anims entrée/boucle/sortie, effets GAS pendant l'usage (dormir = rest Phase 7, manger, forger/alchimie = crafting P1). Lits/chaises/bancs/fours/enclumes/tables d'alchimie. C'est ce qui rend les emplois du temps VISIBLES. |
| **Économie/marchands 3D** | P1 | Port du barter 2D ; inventaires de marchands régénérés (game clock), richesse limitée. |
| **Crime & prime** | P1 | Témoins (perception), bounty par faction (relations existantes), gardes qui arrêtent, prison/amende. |
| **Furtivité** | P1 | Détection (lumière P2, bruit, ligne de vue), état Sneak, dégâts sournois — la Phase 9 (stats) la prévoit. |
| **Repos/attente 3D** | P0 | Lits/chaises via furniture markers, menu d'attente (rest existant Phase 7). |
| **Progression** | P1 | **Décidé (2026-07-05) : skills-by-use** — chaque usage d'ability/effet émet un événement d'usage → XP de compétence → seuils = GameplayEffects passifs. Cohérent avec la courbe d'érudition (STATS.md) ; perks = effets passifs (déjà le modèle). |

## G. UI (jeu) + éditeur d'UI

| Fonctionnalité | Prio | Notes |
|---|---|---|
| **Moteur UI : RmlUi (DÉCIDÉ 2026-07-05)** | P0 | Documents RML/RCSS servis via le **VFS de plugins** → moddabilité au niveau document (le modèle SkyUI/Scaleform : un mod remplace un écran). Adaptateur de rendu sur le RHI, data binding vers l'état de jeu. Les outils dev restent ImGui. |
| **Rendu texte** | P0 | Couvert par RmlUi (FreeType) pour les écrans. Reste un besoin léger de texte monde (nameplates) : overlay screen-space RmlUi ou quads texturés simples. UTF-8 partout (localisation-ready). |
| **Écrans de jeu** | P0→P1 | P0 : HUD (santé/energie/essence/posture — stats existantes, boussole, prompts, crosshair), inventaire/équipement, dialogue, conteneur/loot, menu principal/pause, journal de quêtes. P1 : carte (monde+locale+marqueurs), barter, feuille de personnage, level-up, options complètes, écrans de chargement avec lore. |
| **Navigation gamepad** | P1 | Focus/navigation directionnelle dans tous les écrans. |
| **UI monde** | P0 | Nameplates/barres de vie ennemis, prompts flottants, textes de dégâts P2. |
| **Éditeur d'UI** | P1 | Édition de layout en jeu (ancres, conteneurs, styles), sauvegarde en données moddables. (P0 : layouts en TOML édités à la main + hot-reload.) |
| **Localisation** | P1 | Tables de chaînes = Forms → **un pack de langue est un plugin** (§5 gratuit). Clés dans dialogues/quêtes/UI dès maintenant (P0 : discipline de clés). |

## H. FX

| Fonctionnalité | Prio | Notes |
|---|---|---|
| **Système de particules** | P0 | Émetteurs en données (`ParticleForm`) : formes d'émission, courbes sur la durée de vie (taille/couleur/vitesse), flipbooks, soft particles (depth copy existante), tri/blend additif-alpha, budget global. CPU d'abord ; GPU compute P2 (l'infra existe désormais). |
| **Éditeur de FX** | P1 | Panneau live : édition des params d'émetteur avec preview immédiate, save en Form. (P0 : TOML + hot-reload — le pattern tuning existant.) |
| **Trails & beams** | P1 | Traînées d'armes (sockets d'anim), rayons de sorts. |
| **FX de matériaux** | P1 | Dissolve, freeze, burn — anim de params de MaterialForm via cues. |
| **Screen FX** | P1 | Vignette de dégâts, overlays d'états (poison/ivresse — statuts Phase 7), hit flash — hooks dans le tonemap existant. |
| **Bibliothèque de base** | P0 | Le stock minimal : hit sparks, sang, soin, feu/torches, fumée, poussière de pas, magie par école. Chaque entrée = Form + cue. |

## I. Audio (miniaudio)

| Fonctionnalité | Prio | Notes |
|---|---|---|
| **Moteur son** | P0 | miniaudio : bus (master/SFX/musique/voix/ambiance), `SoundForm` (variations aléatoires, pitch/volume jitter — RNG seedé pour le gameplay, libre pour le cosmétique), spatialisation 3D + atténuation, streaming musique. |
| **Ambiances** | P0 | Beds par région/cellule/météo/heure (la météo pilote déjà tout le visuel — brancher l'audio dessus), transitions fondues (le crossfade météo existe). |
| **Musique dynamique** | P1 | Couches explore/combat/danger avec transitions (tags GAS `State.InCombat` déjà là). |
| **Footsteps par matériau** | P1 | Événements d'anim × matériau du sol (splat weights/type de sol connus). |
| **Reverb par volume** | P1 | Zones de reverb (intérieurs/grottes). |
| **Voix** | P2 | Hooks de playback par ligne de dialogue (subtitle-first pour la démo). |

## J. Données, plugins & outils transverses

| Fonctionnalité | Prio | Notes |
|---|---|---|
| **GameDB browser (l'outil central)** | P0 | Navigateur de TOUS les Forms par type : recherche/filtre, édition par réflexion (les property panels sont déjà prévus §2.3), cross-références (« utilisé par »), création/duplication, **sortie = plugins**. Base de l'éditeur de niveau, de l'éditeur de quêtes, etc. Étendre le WorldEditor embryonnaire. |
| **Gestionnaire de plugins interne** | P0 | UI de configuration : load order, activer/désactiver, dépendances (GUID — déjà le modèle), **rapport de conflits par champ** (le résolveur les détecte déjà), à terme l'outil de synthesis §5.1. |
| **Console développeur** | P0 | La console Skyrim : commandes par réflexion (`set <form>.<field>`, `spawn`, `tgm`, `tcl`, `teleport`, `setstage`), REPL Lua (VM existante). Accélérateur de dev/debug énorme pour un coût faible. |
| **Pipeline d'assets** | P0 | Import glTF complet (meshes/skins/anims/matériaux), KTX2/Basis, extension du cooker (binaire pour tous les nouveaux Forms), **asset browser** avec previews, hot-reload étendu (meshes/anims comme les textures/shaders actuels). |
| **Éditeur de quêtes/dialogues** | P1 | Vue graphe des stages/branches de dialogue sur les records existants (Phase 4) ; P0 = TOML à la main (déjà le cas). |
| **Profiler** | P1 | Zones CPU + timers GPU par passe, compteurs (draws/instances/chunks — partiels), graphe de frame ; P0 minimal = timers par système dans le panneau. |
| **Validation de mods** | P1 | `tools/` : lint d'un plugin (GUIDs, champs inconnus, refs cassées), déjà prévu au CLAUDE.md. |
| **Docs moddeur générées** | P2 | Générer la référence des champs depuis la réflexion (le MODDING-EFFECTS.md à la main ne scalera pas). |
| **Doc utilisateur/moddeur `userdoc/`** | P0 (fait 2026-07-05, à maintenir) | Hub `userdoc/README.md` + pages thématiques liées (plugins, load order, data model, effets, monde, schedules/mobilier, localisation, UI, outils). **Chaque vertical livré met à jour sa page.** |
| **Index secondaires FormDatabase** | P1 | Décision 2026-07-05 : PAS de base SQL au runtime — la FormDatabase résolue EST la base (lookups GUID O(1)). La scalabilité vient de : (a) le format binaire cuit (Phase 1) pour le temps de chargement quand les fichiers se multiplient, (b) des index en mémoire construits après resolve (par type, par `parent`) pour remplacer les scans de `forEach`/`childrenOf` quand le volume le justifiera. SQL n'apporterait que de la friction (dep, mismatch avec la réflexion/le layering §5, lookups plus lents qu'une hashmap). |
| **Crash handling** | P2 | Minidumps + log de la pile de plugins active. |

## K. Sauvegarde & streaming (rappel Phase 10)

Save = couche de patches runtime (§5, non négociable) : état des références
(positions, morts, inventaires), stages de quêtes, journal, temps/météo,
état du joueur. Le streaming 3D (A) et la save sont le MÊME chantier que la
Phase 10 du CLAUDE.md — inchangé, juste re-priorisé P0.

---

## Décisions actées (2026-07-05, avec le dev)

1. **Phase 8.5 (validation Godot) : REPORTÉE après la démo.** La démo de
   gameplay se fait dans Meadows ; le seam sim/présentation (§2.10) reste
   un invariant protégé par les tests headless — Godot redevient une
   option post-démo, rien n'est brûlé.
2. **UI : RmlUi**, documents servis via le VFS de plugins (moddabilité au
   niveau document — le modèle SkyUI/Scaleform). Outils dev = ImGui.
3. **Root motion : NON** — contrôleur cinématique + anims in-place,
   playback rate calé sur la vélocité contre le foot-sliding.
4. **Progression : skills-by-use** (événements d'usage → XP de compétence
   → seuils = GameplayEffects passifs).
5. **Textures : OUI au chantier 1** — albédo plat stylisé + rampe pour
   meshes/personnages ; le paysage garde splat/vertex color.
6. **Le monde est FAIT MAIN, pas généré.** Le procédural n'existe que
   comme outils d'assistance à l'authoring (brosses de scatter, seed de
   terrain de départ, générateur de bases de donjons par règles — sortie
   toujours = records/prefabs retouchables), jamais comme système runtime
   du monde canonique.

## Passe horizontale Fable (5 → 7 juillet 2026) — ✅ FAITE (2026-07-06)

> **Livrée intégralement : voir `docs/HORIZONTAL-PASS.md`** — l'état des
> 8 briques (H1-H8), les règles transverses, les notes « comment remplir »
> par module et les pièges connus. 229 tests verts. Les sessions post-7/07
> exécutent l'ordre macro ci-dessous en suivant ce document.

> **Stratégie de développement (directive dev)** : l'abonnement Fable
> expire le 7 juillet. D'ici là, **Fable pose les bases et l'architecture
> de tous les systèmes, horizontalement** — modules, interfaces, seams,
> types de Forms, squelettes qui compilent — sans détailler les
> implémentations. **Après le 7/07, un modèle moins puissant remplit les
> verticales une par une** en suivant l'architecture posée : suivre les
> seams, ne pas les re-décider, une verticale à la fois, cadence brique
> par brique inchangée. Chaque squelette laisse des commentaires
> d'intention et une note « comment remplir ».

Périmètre de la passe, priorisé par « coût d'une mauvaise décision » :

1. **Modèle de données complet (le cœur)** : déclarer + réfléchir + faire
   résoudre TOUS les nouveaux types de Forms — MaterialForm, LightForm,
   PrefabForm, marker/trigger Forms, CellForm 3D/intérieur,
   AnimClipForm/AnimGraphForm, AppearanceForm + slots, SoundForm,
   ParticleForm, AiPackageForm, ScheduleForm, FurnitureForm,
   LocalizationForm, refs de documents UI — avec doctests de résolution
   §5. (La demande explicite du dev : « travailler mieux mon modèle de
   données et de plugins ».)
2. **Seams de modules** (libs qui compilent, interfaces étroites, impls
   minimales) : `engine/anim/` (Skeleton/Clip/Sampler/Graph, évaluation
   headless-testable), `engine/physics/` (façade Jolt : monde, character
   controller, casts — dep CPM branchée), `engine/ui/` (adaptateur RmlUi
   sur RHI + documents via VFS plugins), `engine/audio/` (façade
   miniaudio : bus, play(SoundForm), 3D), `engine/fx/` (registre
   **GameplayCues** tag→handler — LE pont sim/présentation, headless
   no-op — + squelette particules), `engine/nav/` (interface navmesh,
   Recast branché ou stub assumé).
3. **Boucle éditeur = auteur de plugins** : session d'édition (dirty
   tracking de records → TomlWriter → plugin), squelette GameDB browser
   (table réflexion-driven), console dev (commandes par réflexion + REPL
   Lua). Prouve le concept central « l'outil écrit des plugins ».
4. **Extensions monde 3D** : worldspace/cell 3D + intérieurs dans les
   Forms, spawner 3D (catégories existantes + lumières/marqueurs),
   prefab = instanciation de groupe.
5. **Renderer gameplay (contrats seulement)** : MaterialForm → pipeline
   texturé stylisé (un mesh texturé à l'écran), chemin skinned mesh
   (structure des palettes de bones), design des lumières locales —
   l'implémentation complète reste post-7/07.

## Ordre macro POST-7/07 — les verticales (chaque chantier = sa propre planification en briques)

1. **Socle 3D gameplay** : matériaux/textures + meshes skinnés + animation
   (C) + Jolt & contrôleur (D) + caméra — « un personnage qui court dans le
   paysage existant ». **Absorbe** : le cœur du « frontend 3D » (ex-Phase 11
   chemin custom) et la **brique renderer 27** (arbres à canopée pleine —
   suppression des cards, spec dans `docs/3D-RENDERER.md`).
2. **Monde habitable** : streaming/cellules 3D + intérieurs + lumières
   locales (A, B) + éditeur de niveau v1 + prefabs — « un village et une
   maison où entrer ». **Absorbe** : les lumières/ombres intérieures
   (ex-Phase 12 chemin custom).
3. **Vivant** : navmesh + packages IA + perception (E) + combat 3D + cues +
   FX/audio P0 (F, H, I) — « la boucle Skyrim : explorer, combattre, parler ».
   **Absorbe le reliquat de la Phase 8** : IA ennemie (chase + attaque,
   portée en 3D), PNJ aubergiste (repos) et marchand (achat/vente) — la
   CombatArena 2D reste le banc d'essai GAS en attendant.
4. **Interfaces** : écrans RmlUi P0 (G) + console + GameDB browser + plugin
   manager (J) — squelettes déjà livrés par la passe horizontale.
5. **Persistance** : streaming & save = couche de patches (K — l'ex-Phase 10
   telle quelle, résolveur Phase 1 réutilisé).
6. Puis les **P1 par valeur** : **stats avancées (ex-Phase 9** — machine
   d'état de combat shaken/critical-weakness/démembrement/bleed-out, stats
   offensives/sociales/utilitaires dérivées, blessures avancées ; design
   complet : `docs/STATS.md`, nécessite la boucle de combat 3D du chantier
   3**)**, quêtes 3D outillées, économie/crime, éditeurs (anim/FX/UI),
   musique dynamique, localisation, et le **polish renderer** (briques
   28-31 : color grading LUT, auto-exposition, cumulonimbus, pluie — spec :
   `docs/3D-RENDERER.md`).

> NB : `docs/3D-RENDERER.md` reste le journal + la spec détaillée des
> briques renderer ; leur *planification* vit désormais ici (27→chantier 1,
> lumières intérieures→chantier 2, 28-31→chantier 6). La validation Godot
> (ex-Phase 8.5) reste une option post-démo
> (`docs/SIMULATION-AND-PRESENTATION.md`).