# CLIFFS — habillage des falaises (chantier 2026-08-05)

Réponse au « comment donner de vraies parois aux versants raides ? » —
étude du Cliff Generator Houdini→UE4 de Lucas Dziura (80.lv) transposée
en interne : géométrie pauvre + masque + tileables partagés, raccord au
pied par teinte sol, détail porté par le matériau. Deux étages décidés :

1. **Étage 1 (CE chantier)** — pans de paroi **instanciés** plaqués sur
   les versants où le poids `cliff` sature. Extension pure de
   VegetationSystem (§2.11) : mêmes pools, même scatter déterministe,
   même occlusion GPU, même collision.
2. **Étage 2 (à décider après essai en jeu)** — meshes de falaise
   générés au bake de tuile (surplombs réels, ruban extrudé le long des
   bandes de falaise détectées, débordement du heightfield autorisé).
   Le brush Scénario (étage 3) réutiliserait le même builder sur des
   formes authored — zéro mécanisme en plus.

## Étage 1 — livré (2026-08-05)

### Catégorie cliff (VegetationSystem)

- **4 slots** en fin de table (`kFirstCliff`, `kVariantCount` 22 → 26 ;
  `GpuOcclusion::kMaxGroups` 80 → 96 pour les (variant, LOD)) :
  - **slots 0-1 = dalles procédurales** (`generateCliffFace`,
    TreeGenerator) : grille frontale déplacée (bruit valeur + terrasses
    de strates + profil ventru), bord replié vers −Z pour enterrer les
    chants, 3 LODs de la même seed (18×12 / 10×7 / 5×4). Le **masque
    vertex** (cavité depuis le déplacement + assombrissement de base +
    bake AO à l'upload) module le matériau — l'équivalent interne du
    masque RGB de Dziura.
  - **slots 2-3 = héros scannés** (Poly Haven CC0 1k : `rock_face_01`,
    `namaqualand_cliff_01`) par le chemin d'override scans existant —
    textured-rigid comme les rochers, yaw seul, enfoncés plus profond.
- **Scatter — la face est MESURÉE, jamais supposée** (retour dev
  2026-08-05 « un pauvre rocher alors que la falaise est bien plus
  grande ») : depuis chaque candidat raide, marche de la ligne de pente
  (pas de 3 m tant que la pente tient) → PIED et CRÊTE du mur ; la
  dalle est dimensionnée sur la chute mesurée (échelle 3-15, dalle
  jusqu'à ~33×22 m — sous le pad d'AABB des chunks) et des **rangées
  s'empilent** (max 3, chevauchement 18 %, jitter latéral anti
  z-fight) quand la face dépasse la plus grande dalle. Dédoublonnage :
  seuls les candidats du tiers bas du mur sèment (les candidats hauts
  de la même ligne de pente sortent). Héros = accents (28 % des
  graines), eux aussi dimensionnés par la mesure (0.30 × hauteur,
  clamp 1.8-6). Bloc à salt neuf, APPEND — le contrat d'ordre des
  tirages des autres catégories est intact. Portée = le fade des
  arbres. Mesure spawn : 177 pièces < 900 m, échelles 3 → 15.
- **Pitch par instance (dalles)** : le lane sway-phase, libre sur les
  props rigides non texturés, porte `-(1 + pitch)` ; tree.vert applique
  R_x(−pitch) AVANT le yaw (la dalle s'adosse au versant — pitch =
  0.55 × angle de pente, clampé 0.85 rad : la face reste plus raide que
  le sol, une falaise pas une rampe). Le caster (shadow_prop.vert) et la
  box de collision (quat R_y(−yaw)·R_x(−pitch)) suivent la même chaîne.
  Ceci solde le différé GRASS-REDO « alignement pente des props ».
- **Matériau dalle** : le chemin bark triplanaire de tree.frag (normale
  packée + parallaxe + SSDM alpha) avec un **3ᵉ jeu bark** (slot 2 =
  `rock_face` d'assets-src, converti jpg 1k dans textures/bark) ; le
  frag tourne la vue et la normale triplanaire par le pitch (varying
  vObjNormal passé en vec4, w = pitch). Les slots héros gardent leur
  groupe texturé (skip des `albedoOverrides` dans
  rebuildTreeBarkGroups).
- **Exclusions de plage corrigées** : les tests `v >= kFirstPlant`
  (pas de caster / hero-LOD 0 / pas de GI) excluent maintenant la
  catégorie cliff — les parois **castent** leurs ombres et gardent le
  rayon LOD normal. GI : toujours skip (les GiProp boxent des formes
  arbre/rocher/buisson — un mur de 15 m mérite sa propre boîte, voir
  différés).
- **Collision** : une box orientée par dalle (extents locaux du builder,
  centre y 0.65×s), box yaw carrée pour les héros — VegetationCollision,
  même ring 3×3.

### À tester par le dev (checklist en fin de session)

Voir le message de session ; en résumé : lecture des versants (densité,
échelles, répétition), plaquage/pitch (pas de dalles flottantes ou en
rampe), matériau (relief triplanaire, strates, POM/SSDM), héros scannés,
ombres portées, collision (glisser contre une paroi), F6 (budget veg),
raccord éboulis/sable au pied.

## Étage 2 — livré (2026-08-05), variante « collée »

Verdict dev sur l'étage 1 : « très très loin du résultat » — le
patchwork de dalles ne fera jamais une surface continue, le scatter n'a
qu'une vue locale, et le heightfield seul n'a pas le profil d'une
falaise. L'étage 2 corrige les trois :

- **D1 — bandes + raidissement au bake** (`CliffBands.{hpp,cpp}`,
  headless, appelé par TileBake après finalize) : masque raide à 8 m
  (‖∇h‖ > 1.25 ≈ 51°, gaté loin de l'eau par wetness), composantes
  connexes ≥ 10 cellules, puis **raidissement du profil en place** —
  remap logistique t^k/(t^k+(1−t)^k) entre pied et crête locaux (sondes
  de ligne de pente), delta lissé à 8 m ré-appliqué en bilinéaire sur la
  grille 2 m : mi-face plus verticale, pied/crête adoucis (le profil
  falaise-sur-talus qu'une érosion lisse ne produit pas). Fenêtres
  voisines : marches locales (≤ 40 m) contre marge partagée ≥ 64 m →
  les deux bakes raidissent la bande commune quasi à l'identique, le
  blend de bord absorbe le reste. **Polylignes de PIED** extraites
  (cellule de pied = voisin 4-connexe plus bas et non-raide,
  amincissement spatial, chaînage plus proche voisin), nœuds =
  pied + crête + direction sortante, clipées au rect de la tuile
  propriétaire (jamais deux rubans sur la bande de recouvrement).
  `TerrainRegion::cliffBands`, format .trg TRG3, `kTileBakeVersion` 36.
  Mesure tuile spawn : 129 bandes, jusqu'à 39 nœuds / 405 m linéaires,
  chutes 130-180 m.
- **D2 — rubans continus** (`CliffSystem.{hpp,cpp}` + `cliff.frag`) :
  par bande, colonnes tous les 4 m le long de la polyligne (directions
  lissées entre nœuds), rangées drapées **collées au terrain raidi**
  (échantillonnage `terrain::height` le long de la ligne de pente
  pied→crête, plafond visible 60 m) + relief sortant strates/bruit
  0.15-1.5 m (toujours saillant — pas de z-fight), coutures enterrées
  (rangée basse sous le talus, crête repliée dans la colline, colonnes
  d'extrémité rentrées). Masque vertex (cavité + ton par strate).
  Matériau : **la couche cliff du splat array en triplanaire**
  (cliff.frag, vertex stage partagé terrain.vert, mêmes bind groups que
  le pass terrain — le mur et le sol ne peuvent pas diverger), même
  chaîne d'éclairage que terrain.frag (stylized/nuages/ombre longue/GI/
  locales/fog) + alpha relief SSDM. Casters d'ombres via le shader
  caster du terrain, AABB par bande pour le frustum. Rebuild au
  republish de la base (main thread, ~10k sommets par tuile).
- **D3 — collision & recadrage** : la collision suit le **heightfield
  raidi** gratuitement (TerrainCollision échantillonne terrain::height) ;
  le relief du ruban (≤ 1.5 m saillant) reste visuel. L'étage 1 est
  recadré : les faces ≥ 9 m appartiennent aux rubans (les dalles n'y
  spawnent plus — z-fight), les dalles gardent les affleurements 4-9 m,
  les héros deviennent des accents de pied rares (10 % des graines).
  Spawn : 177 → 10 pièces instanciées.

### Débogage visuel (2026-08-06) — leçons

Le « je ne vois rien de spécial » du dev a été résolu par itération de
CAPTURES D'ÉCRAN pilotées : hooks dev `MEADOWS_AUTOPLAY` (saute le menu
en Play à la fin du warmup) et `MEADOWS_SPAWN="x z yawDeg"` (téléporte
le spawn) — lancement + screencapture scriptés, ~60 s par itération.
Trois causes réelles corrigées :

1. **Winding arbitraire** : le chaînage plus-proche-voisin de la
   polyligne rend le sens de parcours quelconque → la moitié des murs
   étaient back-face culled. Un quad médian est sondé contre la
   direction sortante, le winding de la bande suit (convention moteur
   vérifiée sur les indices du terrain : front = côté du cross dans
   l'ordre des indices).
2. **Plafond 60 m** : sur une face de 145 m, le ruban s'arrêtait en
   pleine pente avec un bord haut rectiligne artificiel. Plafond 200 m
   (44 rangées max, le pas s'étire).
3. **Fréquence de texture unique** : le tiling du sol proche mippe en
   aplat à 300 m. Bi-fréquence dans cliff.frag (octave macro 0.09×,
   fondu par distance, normales incluses) + relief géométrique
   proportionnel à la taille de la face.

Et LE trou systémique : le scatter cédait les faces ≥ 9 m aux rubans
par SEUIL alors que la détection de bandes (51°, gate wetness,
minCells) n'avait pas retenu certaines de ces faces → ni dalles ni
ruban. Corrigé des deux côtés : la détection descend à ~48°
(`slopeGrad` 1.12, v37, 129 → 262 bandes sur la tuile spawn) et le
scatter interroge les **polylignes réelles** de la région (< 20 m d'un
nœud = face au ruban) — une face raide sans bande retombe TOUJOURS sur
les dalles (spawn : 77 dalles + 21 héros).

## Différés

- **Étage 2b — surplombs** : le ruban est collé (drapé sur le
  heightfield raidi) ; autoriser le débordement (vraie verticalité,
  encorbellements) = amplitude de relief signée + carve du heightfield
  sous surplomb + collision mesh Jolt dédiée. Brush Scénario (étage 3)
  réutilise le même builder sur des formes authored.
- Rubans absents du pass réflexion (les montagnes s'y reflètent via le
  terrain seul) et de FarTerrain (la silhouette raidie du heightfield
  porte au loin).
- Relief du ruban ≤ 1.5 m saillant sans collision propre (le joueur
  glisse sur le heightfield en dessous) — mesh Jolt grossier si ça se
  sent en jeu.
- Composantes raides < 10 cellules mais ≥ 9 m : ni ruban ni dalles
  (rare — élargir la passe dalles si des trous se voient).
- Hex-tiling / variantes rock de la couche cliff dans cliff.frag (v1 :
  couche 4 seule + drift de luminance anti-répétition).
- GI des parois (GiProp kind dédié, boîte murale).
- Imposteurs de parois au-delà du ring veg (FarTerrain porte la
  silhouette de pente en attendant).
- Ancrage sol des dalles : la bande 0-0.4 m du contrat H4 est petite
  sur un mur de 12 m — bande proportionnelle à l'échelle si le pied
  jure (le tablier d'éboulis couvre l'essentiel).
- Variantes lithologiques des strates (lier `strataBands`/`strataAmp`
  au champ lithologie de la génération).
- BC7 des textures de paroi (avec le lot props différé de GRASS-REDO).
