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

## Différés

- **Étage 2** : détection de bandes au bake + ruban extrudé à surplombs
  (go/no-go dev après l'étage 1) ; carve du heightfield sous surplomb ;
  brush Scénario (étage 3).
- GI des parois (GiProp kind dédié, boîte murale).
- Imposteurs de parois au-delà du ring veg (FarTerrain porte la
  silhouette de pente en attendant).
- Ancrage sol des dalles : la bande 0-0.4 m du contrat H4 est petite
  sur un mur de 12 m — bande proportionnelle à l'échelle si le pied
  jure (le tablier d'éboulis couvre l'essentiel).
- Variantes lithologiques des strates (lier `strataBands`/`strataAmp`
  au champ lithologie de la génération).
- BC7 des textures de paroi (avec le lot props différé de GRASS-REDO).
