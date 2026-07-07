# Chantier 7 — Graphisme : ce qui reste

> **FAIT (2026-07-07) — briques 7.1-7.7 exécutées d'une traite sur le go
> du dev (l'herbe 7.8 exclue, réf. reçue : daniel-ilett/shaders-botw-grass
> — session future).** 276 tests verts, smoke-run par brique, builds
> Debug + Release. VALIDATION VISUELLE DEV EN ATTENTE sur les 7 briques.
> Specs détaillées : `docs/3D-RENDERER.md` (briques 30-34). Leçons :
> FrameUbo n'a PLUS de .w libres — trois APPENDs de fin de struct posés
> ce chantier (uTerrainLightInfo, uSubmersionInfo, keyShadow*, stormInfo,
> rainOcclusionViewProj) ; purge *.dir à chaque layout partagé (fait ×5).

## Contexte

La passe lighting du chantier 6 (axe B : spots+falloff inverse-square,
casters mesh/PNJ dans le CSM, grading 28, auto-expo 29, passe intérieure
B5 : hémisphérique + wrap/bounce + fix submersion + hall d'essai) est
**validée et commitée par le dev (2026-07-07)**. Restent les briques
spécifiées mais non construites, plus le backlog météo.

## Ce qui a été livré (résumé d'exécution)

- **7.1 Shafts (34)** : prismes additifs procéduraux depuis LightForm
  (APPEND shaft/shaftLength/shaftSoftness/dustDensity/sunLinked) ; bruit
  défilant + motes ; `sunLinked` suit le soleil quantifié des ombres.
  Banc : HallWindowShaft (mur est du hall, kind=spot → éclaire ET montre).
- **7.2 Contact shadows (33a)** : marche Bend 12 pas demi-res sur la
  depth copy (clone SSAO), assombrit 45 % max ; toggle = clear-blanc.
- **7.3 TerrainLightMap (33b/c)** : bake worker 256²/1,5 km, R =
  visibilité soleil (marche géométrique ~1 km), G = ouverture ciel (8
  horizons) ; re-bake au pas du soleil quantifié ; texture unit 7, slot 4.
- **7.4 Eau plaçable (32)** : WaterVolumeForm → FormCategory::Water →
  WaterVolume ; quad de surface stylisé (fresnel ciel + rides) ;
  submersion généralisée (uSubmersionInfo = surface effective — mer /
  sommet de volume / sec). Banc : bande inondée du fond du hall (y = 1).
- **7.5 Key light shadow (B2b)** : 1 layer Depth32F 1024² perspective
  depuis la lumière castsShadow la plus proche (intérieur) ; refactor
  drawCastersInto ; match par position dans locallights (unit 6, slot 5).
  TableSpot caste dans la pièce d'entrée.
- **7.6 Cumulonimbus (30)** : 8 billboards ancrés caméra à 3,2 km, FBM
  érodé, cel 2 tons + silver lining ; WeatherForm.stormFront crossfadé
  (Storm = 1).
- **7.7 Pluie (31 + addendum)** : 3000 streaks 100 % procéduraux
  (gl_VertexID, zéro buffer/sim), occlusion top-down 512² (pas de pluie
  sous les toits — réutilise drawCastersInto), wetness globale (terrain
  28 %, meshes 25 %, extérieur seulement) ; WeatherForm.rainIntensity +
  état « Rain » ; per-pixel dry-under-cover = raffinement noté.

## Les briques, par valeur (ordre proposé)

| # | Brique | Spec | Notes |
|---|--------|------|-------|
| 7.1 | **34 — Light shafts à poussière** (FXShaft Skyrim, sunLinked) | 3D-RENDERER §34 | Demande dev explicite ; banc d'essai = une fenêtre du hall ; règle aussi le rendu du cône des spots |
| 7.2 | **33a — Contact shadows screen-space** (Bend) | 3D-RENDERER §33 | Clone du pattern SSAO ; toggle par clear-blanc (zéro slot .w libre) |
| 7.3 | **33b/c — Ombres de terrain lointaines + skylighting** | 3D-RENDERER §33 | UN bake worker 256² 2 canaux (pattern cloud-map), re-bake sur le pas d'hystérésis du soleil ; APPEND FrameUbo |
| 7.4 | **32 — Eau plaçable (`WaterVolumeForm`)** | 3D-RENDERER §32 | Lacs d'altitude + grottes inondées ; la mer garde le miroir |
| 7.5 | **B2b — Ombre de lumière clé intérieure** | CHANTIER-6.md (descope) | KeyLightShadow perspective 1 layer ; stoppe le light bleed à travers les murs — utile dès les intérieurs multi-pièces |
| 7.6 | **30 — Cumulonimbus à l'horizon** | 3D-RENDERER §30 | `WeatherForm.stormFront` ; billboards géants |
| 7.7 | **31 — Pluie + wetness + occlusion top-down** | 3D-RENDERER §31 + addendum | Particules cylindre caméra ; depth map top-down (la pluie s'arrête sous les toits) |
| 7.8 | **Refonte herbe** | backlog 3D-RENDERER | pour les herbes tu peux utiliser https://github.com/daniel-ilett/shaders-botw-grass


Descopés/backlog (inchangés) : LUT 3D, TAA, caustiques, biomes, mode
dégradé GL 4.1, impostors, 3e personne.

## Chantiers suivants (esquisse d'après les P1/P2 restants de MEADOWS-PLAN)

À trancher avec le dev au moment voulu — l'ordre ci-dessous est une
proposition par valeur pour la démo :

- **Chantier 8 — Outillage** : éditeur de quêtes (le runtime est complet,
  l'outillage manque), timeline des schedules (P1 promis §6 décisions),
  éditeurs anim/FX/UI, outil de synthesis patch (§5.1 CLAUDE.md —
  prérequis : FieldConflict porte les valeurs).
- **Chantier 9 — Confort & plateforme** : gamepad, localisation
  systématique (l'infra existe), carte en jeu, async IO réel des saves/
  cellules (fichiers cuits), build Linux re-vérifié (§3.1 : les deux OS
  doivent rester buildables).
- **Chantier 10 — Son & vie** (APRÈS confort & plateforme — ordre dev
  2026-07-07) : assets audio À DÉPOSER PAR LE DEV (bloquant), câblage
  cues→miniaudio (seams posés), musique dynamique (states calme/
  exploration/combat via tags), bruits de pas par matériau, reliquat IA
  vivante (marchand ambulant ? combat PNJ affiné), Recast en remplacement
  du TerrainNavigator (les PNJ traversent les troncs depuis la collision
  végétation).
- **Post-démo** (rappels) : validation Godot (ex-8.5), followers
  (contrat posé chantier 5), table de relations factions, démembrement/
  bleed-out (assets gore), vue 3e personne.

## Quoi tester par brique

Chaque brique reprend le critère de validation de sa spec dans
3D-RENDERER.md ; l'extérieur ne doit JAMAIS changer toggles off
(byte-identique), et le hall d'essai du chantier 6 reste le banc intérieur
(boot direct : `kDevStartInterior` dans LandscapeScene.cpp).
