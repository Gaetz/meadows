# CHANTIER 1 — Socle 3D gameplay (FAIT, 2026-07-06)

> Journal de briques (précédent : `docs/PHASE-*.md`). Livré : joueur
> première personne piloté par ses stats + PNJ skinné/animé 100 % Forms
> qui patrouille dans le paysage + arbres à canopée pleine (brique
> renderer 27). 234 tests / 77 752 assertions verts à la clôture.

## Décisions actées pendant le chantier

- **Le jeu est à la PREMIÈRE personne** (le joueur = capsule + caméra aux
  yeux, pas de mesh visible en v1) ; la 3e personne sert aux PNJ.
  MEADOWS-PLAN §B (ligne caméra) et §C.1 mis à jour.
- **Étude NarrativePro** (MEADOWS-PLAN §C.1) : `ActorForm` EST la
  character definition ; les manques (grants, loadout, tags par défaut,
  dialogue, vendeur, variations d'apparence) sont planifiés par verticale.
- Asset PNJ : **Quaternius UAL mannequin** (CC0, 43 clips IN-PLACE, 65 os,
  mètres) déposé par le dev ; branché en repointant l'asset guid + 3
  `animationName` — zéro changement de records de graphe (§5 prouvé).

## Les briques

- **B1 — chemin mesh réel** : `game/MeshCache` (résidence async, pattern
  TextureCache, placeholder box magenta = « loading » visible ;
  `assets::groundMesh` = convention pivot des props), consommation de
  `RenderSnapshot.meshes` (l'écart n°3 de l'audit HORIZONTAL-PASS est
  CLOS), plugin `adventure.toml`, spawn des ReferenceForms par le Spawner,
  suppression du cube H8. `extractMeshes` extrait de `extractScene`
  (doctestable sans GPU). TextureCache : `UploadDesc` (SRGBA8+Linear pour
  l'albédo 3D). Convention B1 : `position.y` autorisé = offset au-dessus
  du terrain (jusqu'à l'éditeur de niveau, chantier 2).
- **B2 — GPU skinning** : `SkinnedVertex/SkinnedMeshData` (os en float —
  le RHI n'a que des formats float), `loadGltfSkinnedMesh` avec **remap
  JOINTS_0 par la MÊME permutation parents-first que loadGltfSkeleton**
  (doctest child-first dédié), `skinned.vert` (palette SSBO binding 2) +
  `skinned.frag` (jumeau de mesh.frag). **Leçon HDR** : teintes d'albédo
  NETTEMENT sous 1.0 sinon le soleil HDR sature et l'ACES écrase au blanc.
- **B3 — chemin data anim** : AnimClip/Graph/State/TransitionForms dans
  adventure.toml → AnimBridge → GraphInstance ; transitions sur le param
  `speed` (montantes greater / descendantes less), referenceSpeed
  anti-foot-sliding ; events Footstep (records enfants). Doctest cycle
  idle→walk→run→walk→idle depuis les records.
- **B4 — collision terrain** : `PhysicsWorld::addHeightField` (Jolt,
  pimpl intact), `game/TerrainCollision` — tuiles 64×64 à 1 m sur leur
  propre grille (63 m, bords partagés), échantillonnées de
  `TerrainNoise::height` (= la fonction du rendu : collision indépendante
  du LOD par construction), anneau 3×3 + éviction hystérésis. Doctests
  chute/pente/raycast + repos à la hauteur exacte du terrain.
- **B5 — joueur première personne** : toggle Fly/Play (touche F), capsule
  1,80 m + caméra à 1,70 m, mouselook capturé, WASD relatif au regard,
  sprint Shift, saut Espace, lissage exponentiel de vitesse.
- **B5.5 — le joueur est un acteur GAS** (demande dev : STATS.md s'applique
  au joueur) : ActorForm "Player" spawné + `tickCharacter` chaque frame ;
  le contrôleur LIT `movementSpeed`/`acceleration` courants
  (kSpeedScale3D = 1/20 après passe de feel dev : jog ~5,1 m/s, sprint
  ×1,6) ; **sprint payé par l'EffectForm SprintCost** (§2.9), coupé à
  énergie vide ; debuff test « TestLegWound » = preuve visible (mi-vitesse
  10 s). Saut constant (jump power = P1 stats).
- **B6 — PNJ 100 % Forms** : `world::resolveActorVisual` (ActorForm →
  appearance → rig + premier slot de mesh + teinte ; doctesté), cache de
  rigs par asset, PNJ « Villager » (AppearanceForm + CharacterGraph +
  ReferenceForm + 2 markers `kind="patrol"`), patrouille aller-retour avec
  pauses 2,5 s, vitesse lue des stats du PNJ, yaw lissé (mannequin face
  +Z), anims par vitesse réelle. La scène ne construit plus rien à la main.
- **B7 — brique renderer 27** : canopées pleines (icosphères subdiv 2,
  jitter 0.10-0.14, aplaties 0.85), **normales sphérisées sur le mesh**
  (mix(dir_lobe, dir_canopée, 0.4)), verts pleins + gradient vertical
  +25 %, stylizedRim dans tree.frag, **suppression totale des leaf cards**.
  **Post-validation perf (30 fps !) : LOD de canopée par chunk** — subdiv 2
  dans un rayon de 4 chunks (~256 m), subdiv 1 (80 faces, même seed/
  silhouette) au-delà ET pour les casters d'ombre ET la réflexion. FPS
  revenus au max.

## Interface (rangée en fin de chantier)

Panneau Landscape en sections repliables synchronisées clic/touche :
**F1** gameplay (joueur/PNJ/physique), **F2** terrain & streaming, **F3**
ciel/météo/heure, **F4** rendu & post-FX, **F10** masque tout le panneau.
Via l'état clavier ImGui (fonctionne souris capturée).

## Pièges payés (ne pas repayer)

- Teinte d'albédo trop claire × soleil HDR → blanc ACES (garde ~0.6 max).
- `updateBuffer` = une écriture par buffer par frame, AVANT les commandes
  qui la consomment (précédent frameUbo).
- GraphInstance référence son GraphDesc : le desc doit avoir une adresse
  stable (uptr<Npc>, membres de scène) et être détruit APRÈS l'instance.
- Poids de vertex JOINTS_0 : toujours remapper par `topologicalJointOrder`
  — sinon palette et squelette divergent silencieusement.
- Jolt HeightFieldShape : sampleCount en puissance de 2 ; l'origine se
  bake dans la shape (body à zéro).
- Assets : quaternius/poly.pizza ne se téléchargent pas en direct (zip au
  navigateur) ; vérifier que les clips d'un pack sont IN-PLACE
  (translations racine ~0 dans le JSON glb) avant adoption.
