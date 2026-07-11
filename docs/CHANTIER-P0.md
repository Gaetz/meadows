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
- **A5 ✅ — Blocage directionnel.** `gameplay::applyBlock` (MeleeSwing) :
  cône avant HORIZONTAL (largeur `blockAngleDegrees` 120°), canaux
  ×(1−`blockFactor` 0.7), le bloqué part en POSTURE ×`blockPostureFactor`
  0.6 — garde brisée = le stagger existant. Les 5 réglages sont des
  champs `StatsTuningForm` (§5, append). Joueur : RMB tenu = tag
  `State.Blocking` + vitesse ×`blockSpeedFactor` 0.5, pas de sprint ni
  d'attaque en garde ; le Transform du joueur gagne enfin sa ROTATION
  (rotation×+Z = regard horizontal, convention yaw PNJ) — le cône lit le
  transform, jamais la caméra. PNJ : roll UNE fois par fenêtre
  inter-swings (`npcBlockChance` 0.35) sur un RNG moteur seedé dédié
  (`combatRng`, §8), miroir tag State.Blocking ; la garde tombe quand il
  frappe ou que le combat cesse. Les DEUX chemins de dégâts passent par
  applyBlock avant applyDamage. Doctesté (dans/hors cône, par-derrière,
  hauteur ignorée, bout portant, réduction + routage posture). Reste
  dev : feel + pas d'anim de garde (pas de clip block dans UAL).
- **A6 ✅ — Distances d'engagement par arme.** L'IA s'arrête et frappe à
  `WeaponForm.reach − 0.3` (BanditClub : reach 2.0 en data) au lieu du
  1,8 codé en dur. La distance de préférence de strafe (reach + 1)
  arrive avec B3.
- **A7 — Projectiles (SI retenu, voir questions).** Entité flèche :
  spawn au socket, balistique simple (gravité + vitesse), impact par
  raycast segmentaire → damage pipeline ; pickup de la flèche plantée P1.

## Axe B — Perception, IA de combat, et l'index spatial partagé

- **B1 ✅ — Index spatial partagé (l'infra promise).**
  `world/scene/SpatialIndex` : hash grid XZ (cellule 4 m), snapshot
  entité+position au `rebuild` (1×/frame scène — la sémantique snapshot
  des triggers est préservée), `queryRadius` / `queryCone` (distances
  3D, bucketing planaire, bout portant = vu). Premier consommateur :
  `updateTriggerVolumes(…, index)` — candidats = rayon englobant du
  volume, et un LEAVE-SWEEP couvre l'acteur qui a quitté le voisinage
  d'un coup (téléport). Doctesté (bords de cellules, cône avant/arrière/
  étroit, parité enter/leave via index). B2 (perception) le consomme
  (remplace le scan linéaire), le pick éditeur ensuite. Doctesté.
- **B2 ✅ — Composant Perception dédié.** `world/ai/Perception` (PAS
  gameplay/ai : meadows-world dépend de meadows-gameplay, jamais
  l'inverse). Composant REFLÉTÉ (un camp alerté le reste à travers une
  save) : viewDistance 14 / viewAngle 140° / hearingRadius 12 /
  memorySeconds 5 / searchSeconds 8 + état/lastKnownPos/timers. Machine
  Calm→Suspicious→Alert→Searching, transitions par UNE fonction
  (`setAwareState`). Vision = `inViewCone` + LOS raycast du CALLER
  (pattern TriggerSystem — world/ ne touche pas la physique de game/) ;
  ouïe = `hearNoise` routé par la scène depuis les events `OnNoise`
  (position = transform de la source ; les ÉMETTEURS arrivent en C4b).
  NpcDirector : le bloc inline distance<16 est remplacé — Alert chasse,
  vue perdue → il ENQUÊTE sur la dernière position connue (marche) puis
  abandonne ; l'attaque exige la VUE. La furtivité P1 s'appuiera dessus.
  Doctesté ×4 (cône, mémoire/dégradation, ouïe à portée seulement,
  re-visée qui relance la patience, Alert sourd au bruit).
- **B3 ✅ — Comportements de combat.**
  `gameplay/combat/CombatAi::chooseCombatMove` : LA décision (enum
  Approach/Strike/Strafe/Flee, une fonction plate sim-pure) —
  NpcDirector ne fait qu'EXÉCUTER. Strafe : orbite tangentielle (sens
  par parité d'id — deux bandits tournent en sens opposés) + dérive
  radiale qui tient le milieu de la bande d'arme [attackRange,
  reach+1], face à la cible, quand l'attaque recharge. Fuite :
  `ActorForm.courage` {0.75, §5 append} — il craque sous (1−courage) de
  santé et court à l'opposé. Appel à l'aide : front montant d'Alert →
  event `CallForHelp` sur le bus + les alliés de MÊME factionTag sous
  `helpCallRadius` {20, StatsTuningForm} reçoivent `alertTo`
  (l'info fraîche du camarade : Alert direct sur la position).
  Doctesté ×2 (matrice de décision portée/cooldown/vue/courage ;
  alertTo = mémoire pleine avant dégradation).

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
