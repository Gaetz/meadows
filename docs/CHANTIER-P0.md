# Chantier « Reliquats P0 » — le vivant 2 (combat, perception, FX, mobilier)

> Plan écrit le 2026-07-11 (go dev : « faisons les reliquats du P0 »).
> Rassemble TOUT ce qui reste marqué 🔨 P0 au catalogue de
> `docs/MEADOWS-PLAN.md` après les chantiers 1-8, GPU-PERF et RC. Quatre
> axes ordonnés par valeur pour la vertical slice ; chaque brique suit la
> cadence habituelle (une brique = build + tests headless verts + commit,
> validation dev entre les briques pour ce qui se voit en jeu).

---

## Ce qui reste, d'où ça vient

| Item catalogue | Reste | Axe |
|---|---|---|
| Combat 3D | ability GAS formelle, fenêtres de hit par AnimEvents (shape cast), blocage directionnel, projectiles, lock-on | A |
| Attach points | sockets sur os (main = arme, dos = fourreau), armes visibles | A |
| Intégration Jolt | shape casts combat | A |
| Perception 3D | composant dédié (cône, ouïe, mémoire de position) | B |
| IA de combat 3D | strafe, fuite (courage/santé), appel à l'aide (factions), distances d'engagement par arme | B |
| Événements sur timeline | consommateurs : fenêtres de dégâts, footsteps, FX, shake | A/C |
| Système de particules | émetteurs continus, formes, courbes, rendu 3D quads caméra, soft particles, budget | C |
| GameplayCues | handlers standard (particule/son/shake) + points d'émission systématiques | C |
| Moteur son | résolveur `SoundForm` (variantes, jitter, AssetDatabase) | C |
| Mobilier | effet GAS pendant l'usage, anims entrée/sortie | D |
| Volumes de gameplay | kill-z, volumes d'eau (nage), zones de son | D |
| Écrans de jeu | journal de quêtes | D |
| Pipeline d'assets | KTX2/Basis, asset browser, hot-reload meshes/anims | E (queue) |

**Différés par design (on ne les fait PAS ici)** : Recast/Detour (« quand
la grille pique » — la grille ne pique pas), sensors Jolt pour triggers
(la boîte analytique a gagné), textes de dégâts (P2), la VALIDATION audio
(bloquée tant que le dev n'a pas déposé d'assets son — le code, lui, se
fait avec le backend null).

## Axe A — Le combat de mêlée complet (le cœur du feel)

- **A1 ✅ (`21626e6`) — Shape casts Jolt.** La façade `phys::` gagne
  `sphereCast` (miroir de rayCast, headless-testé). Sert aux PROJECTILES
  vs monde statique — pas aux hits d'acteurs : les `CharacterVirtual`
  sont HORS broadphase, un cast physique ne les voit pas.
- **A2 ✅ (`af51d62`, + fix boot `a23b5d6`) — L'épée visible et son
  socket (design dev 2026-07-11).** Asset épée PROCÉDURAL simple
  (lame+garde+poignée, `game/WeaponMeshes`, lame sur +Y depuis la
  poignée) injecté au MeshCache (`injectProcedural`) ; `WeaponForm` +=
  champs combat (bladeLength/hitTolerance/timings/reach/projectileSpeed).
  DÉCOUVERTE : le rig UAL A un clip `Sword_Attack` (1,533 s) + le joint
  `hand_r` — les PNJ hostiles portent l'épée dans la main (extract =
  `anim::modelMatrices`), le joueur a son viewmodel caméra. Leçon boot :
  la pose est bind-initialisée à la construction (l'extract tourne en
  Spectator où update() ne passe pas). Reste dev : correction de grip
  éventuelle (~90°) dans la main du bandit.
- **A3+A4 ✅ — Ability GAS + hit À LA LAME.**
  `gameplay/combat/MeleeSwing` : machine Idle→Windup→Active→Recovery
  (enum + UNE fonction de transition `setSwingPhase`), timings =
  `WeaponForm` (BanditClub calé sur le clip : 0.55/0.40/0.58) ;
  activation = `tryActivate` de l'ability "PlayerAttack" PARTAGÉE
  joueur/PNJ (coût énergie + cooldown en effects, §6). **Le hit = la
  lame TOUCHE** : pendant Active, segment de lame (× `hitTolerance`)
  contre capsule ANALYTIQUE (`segmentHitsCapsule`, Ericson — les
  CharacterVirtual sont hors broadphase), une touche par swing
  (`registerStrike`) → `weaponDamageEvent` → `applyDamage` (passe crime
  conservée). PNJ : le graph data gagne l'état `CharStateAttack`
  (Sword_Attack, gate `State.Attacking`, transitions APRÈS la mort dans
  le fichier = priorité) ; les AnimEvents authorés `HitOpen` 0.55 /
  `HitClose` 0.95 pilotent la fenêtre via le sink C4a ; la lame de hit =
  la lame VISIBLE (world × hand_r × +Y). Joueur : viewmodel balayé par
  `swingSocketLocal` (garde → armé droite → balayage → retour), le même
  socket que le test de hit. Doctesté ×5 (phases, override d'events,
  dédup, arc droite→gauche, segment-capsule + tolérance). Reste dev :
  feel (fenêtres du clip, arc du viewmodel, capsule 0.4 [cpp-tuning]).
- **A5 — Blocage directionnel.** État bloqué (RMB / ability PNJ) : les
  coups entrants dans le cône avant → dégâts réduits + dégâts de POSTURE
  (le système posture/stagger de STATS.md est déjà là), garde brisée =
  stagger. Doctesté (cônes, réductions).
- **A6 — Distances d'engagement par arme.** `WeaponForm.reach` (+ champ
  de préférence de distance) consommé par l'IA (B3) et par le cast A4.
- **A7 — Projectiles (SI retenu, voir questions).** Entité flèche :
  spawn au socket, balistique simple (gravité + vitesse), impact par
  raycast segmentaire → damage pipeline ; pickup de la flèche plantée P1.

## Axe B — Perception, IA de combat, et l'index spatial partagé

- **B1 — Index spatial partagé (l'infra promise).** `world/scene/`
  gagne une grille spatiale d'entités (hash grid AABB, rebuild/maj par
  frame — simple avant octree) avec des requêtes `queryRadius`,
  `queryCone`. Consommateurs immédiats : B2 (perception), TriggerSystem
  (remplace le scan linéaire), le pick éditeur ensuite. Doctesté.
- **B2 — Composant Perception dédié.** Extrait de l'inline NpcDirector :
  cône de vue (angle/portée) + LOS raycast, OUÏE (les événements bruit —
  combat, pas, sprint — émis sur l'EventBus avec position/rayon),
  mémoire de dernière position connue + états alerte
  (calme → suspicion → alerte → recherche). La furtivité P1 s'appuiera
  dessus. Headless : cônes, occlusion, décroissance mémoire.
- **B3 — Comportements de combat.** Sur la perception B2 et les
  distances A6 : strafe autour de la cible à portée, fuite quand
  courage/santé bas (attributs GAS — le seuil est un Form), appel à
  l'aide (broadcast aux alliés de faction dans un rayon via B1 → ils
  passent en alerte). Doctesté en sim.

## Axe C — FX, cues et les consommateurs d'AnimEvents

- **C1 — Particules v2.** `fx::ParticleSim` : émetteurs continus (rate),
  formes (sphere/cone/box), courbes taille/couleur sur la vie ; RENDU 3D
  en quads face caméra (instancié, soft particles contre le z-fight sol),
  budget global. L'éditeur FX (8.10) preview déjà — il suit gratuitement.
- **C2 — Handlers de cues standard.** Le `CueForm` résout enfin ses
  trois canaux : particule (C1), SHAKE caméra (impulsion amortie), son
  (C3 — silencieux tant que pas d'assets). Points d'émission
  systématiques : hit mêlée, mort, block/garde brisée, pas (C4).
- **C3 — Résolveur `SoundForm`.** Variantes pondérées + jitter
  pitch/volume + chemin AssetDatabase → l'API play du seam H6. Headless
  avec le backend null ; la validation audio attend le dépôt d'assets.
- **C4 — Footsteps par matériau.** L'AnimEvent « Footstep » (déjà sur le
  cycle de marche) → matériau sous le pied (poids splat / intérieur) →
  cue `Cue.Footstep.<Material>` : poussière FX tout de suite, son quand
  les assets arrivent.

## Axe D — Mobilier, volumes, journal

- **D1 — Mobilier : effet GAS pendant l'usage + anims d'entrée/sortie.**
  `FurnitureForm.useEffect` appliqué à l'occupation, retiré à la sortie
  (repos = regen boostée — les effects existent) ; transitions assis/
  debout par états d'anim au claim/release.
- **D2 — Kill-z + volumes d'eau v1.** Kill-z : seuil worldspace
  (sémantique à trancher, voir questions). Eau : le `WaterVolumeInstance`
  existe côté rendu — v1 gameplay = flottaison/nage simple OU dégâts/
  respawn (à trancher).
- **D3 — Journal de quêtes.** L'écran P0 restant (chantier 4 pattern
  RmlUi) : quêtes actives/terminées + étapes depuis l'état quest
  existant.

## Axe E — Pipeline d'assets (queue de chantier, à re-prioriser en fin)

KTX2/Basis, asset browser avec previews, hot-reload meshes/anims — de
l'outillage confort : à faire si le chantier se plie vite, sinon re-passe
au catalogue.

## Ordre proposé

A1 → A2 → A3 → A4 (le combat sent bon) → A5/A6 → B1 → B2 → B3 (les PNJ
répondent) → C1 → C2 → C4 (ça claque à l'écran) → C3 → D1 → D2 → D3 → A7
si retenu → E si le temps.

## Cadrage TRANCHÉ (dev, 2026-07-11)

1. **Kill-z = MORT FRANCHE** : dégâts massifs par le pipeline GAS →
   mort normale.
2. **Projectiles (A7) : RETENUS** dans ce chantier (flèche balistique,
   ennemi archer possible).
3. **Lock-on : ABANDONNÉ** (pattern 3e personne ; le cône de mêlée
   suffit en vue subjective). Rayé du catalogue.
4. **Nage : VRAIE NAGE maintenant.** D2 s'étend : détection d'eau
   (WaterVolume + mer), flottaison + contrôleur de nage première
   personne (la submersion visuelle — brique 32 — existe déjà), drain
   d'énergie par effect, noyade à énergie nulle → dégâts. Anims de nage
   PNJ : différées P1 (le joueur est en vue subjective).
5. **Assets audio** : quand le dev dépose des sons (même 3-4
   placeholders CC0 : pas, coup, porte), C3/C4 se valident.

## Vérification

Par brique : build + suite headless complète verte (les axes A/B/D sont
sim purs — doctests obligatoires ; C1 rendu = validation visuelle dev) +
smoke-run. Les briques combat se valident dans la CombatArena 2D quand le
concept le permet (le banc GAS) puis en 3D.
