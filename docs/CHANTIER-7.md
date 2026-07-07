# Chantier 7 — Graphisme : ce qui reste (PLAN, ouvert 2026-07-07)

> **PLAN — pas encore exécuté.** Ce fichier devient le journal au fil des
> briques (le pattern des chantiers 1-6). Toutes les specs détaillées
> vivent dans `docs/3D-RENDERER.md` (briques 30-34 + backlog) — CE
> fichier ordonne et scope ; LA spec fait foi. Règles héritées :
> **une brique = build + tests verts + validation visuelle dev avant la
> suivante** ; tout changement du look extérieur derrière un toggle A/B ;
> leçon UBO (APPEND en fin de struct, les .w libres de FrameUbo sont
> ÉPUISÉS — inventaire dans CHANTIER-6.md) ; purge des *.dir à chaque
> layout de type partagé.

## Contexte

La passe lighting du chantier 6 (axe B : spots+falloff inverse-square,
casters mesh/PNJ dans le CSM, grading 28, auto-expo 29, passe intérieure
B5 : hémisphérique + wrap/bounce + fix submersion + hall d'essai) est
**validée et commitée par le dev (2026-07-07)**. Restent les briques
spécifiées mais non construites, plus le backlog météo.

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
