# LIGHTING — architecture des lumières & politique d'ombres

> La réflexion d'architecture (2026-07-24, demandée par le dev après le
> chantier VOLUMETRIC) : ce que le forward actuel est, comment intérieur
> et extérieur partagent les algorithmes, les limites de nombre de
> lumières, pourquoi le deferred est écarté, et la trajectoire décidée.
> Les ombres de paysage (CSM, terrain light map) vivent dans
> `docs/3D-RENDERER.md` ; la GI dans `docs/RADIANCE-CASCADES.md` ; le
> volumétrique dans `docs/VOLUMETRIC.md`.

## 1. L'architecture actuelle : forward single-pass, partout

Tous les shaders de surface (terrain, mesh, skinned, grass) rendent en
UNE passe et bouclent par pixel sur le tableau des lumières locales
(`LightsUbo`). Il n'y a **aucune différence d'algorithme entre intérieur
et extérieur** — ce qui change est paramétrique :

| Système | Extérieur | Intérieur |
|---|---|---|
| Soleil + CSM | ✅ (3 cascades, 800 m) | coupé |
| Ambient | ciel × terrain light map | `interiorAmbient` × H1 (heure × météo × enterré) |
| Lumières locales | mêmes slots, meshes/persos seulement | + wrap diffuse, bounce omni |
| GI Radiance Cascades | ✅ (fenêtre 32 m) | ✅ (boîtes kit + blobs portent la pièce) |
| Froxel fog | ✅ (800 m) | ✅ (poussière, 48 m) |
| Key shadow | — | ✅ 1024² perspective, 1 lumière |
| Fenêtres sun-linked | (délégué au volumétrique) | ✅ le soleil ré-injecté par la donnée |

L'asymétrie de fond : dehors, la « grande lumière ombrée » est le
**soleil** (la CSM est sa map) et les locales sont décoratives ; dedans,
les locales SONT l'éclairage — c'est là que nombre et ombres comptent.

## 2. Le nombre de lumières

**Budget : 64** (2026-07-24, chantier §5 — était 24, avant 16), sélection
**frustum + importance** (`intensity/(1+dist²)`, cull sphère-frustum,
liste finale ordonnée par distance — `SceneSubmit`), une liste unique
pour trois consommateurs : le direct (LightsUbo), les blobs GI (RC, les
24 plus proches — fenêtre ~32 m), les froxels. Modèle de coût du
forward : `coût ≈ pixels × slots`. Le plein budget n'est consommable que
par le chemin clustered ; la boucle legacy (et la passe reflet) reste
clampée à 24.

- ✅ **Clustered forward (Forward+) — FAIT (2026-07-24, §5)** : un
  compute (`cluster_cull.comp`, `LightClusters`) assigne les lumières
  aux cellules d'une grille 16×9×64 dont les tranches Z SONT celles des
  froxels (include partagé `clusters.glsl`) ; surfaces ET
  `froxel_inject` ne bouclent que sur la liste de leur cellule (SSBO,
  binding 4 du frame group). A/B : checkbox « Clustered lights »,
  fallback = la boucle 24 historique (aussi le chemin sans compute).
- **Deferred : écarté.** Raisons consignées pour ne pas rouvrir sans
  élément nouveau : (1) la stylisation est par TYPE de surface (splat,
  rampes cel, herbe, cutout des cartes) — G-buffer obèse ou shader-IDs,
  et cutout/transparence redeviennent des cas spéciaux ; (2) sur M1 le
  deferred classique paie le G-buffer en bande passante SANS l'avantage
  tile-memory (MoltenVK n'expose pas le tile shading via Vulkan) ;
  (3) le clustered forward donne la même scalabilité sans refactor des
  shaders. Le deferred résout un problème que le clustered résout mieux
  pour CE moteur.

## 3. La politique d'ombres intérieure

Trois étages, du plus précis au plus diffus :

1. **Fenêtres = projecteurs rectangle** (2026-07-24) : un faisceau de
   fenêtre est LE rectangle du cadre extrudé le long du soleil vivant —
   l'occlusion de l'ouverture est de la DONNÉE
   (`LightForm.windowHalfWidth/Height` > 0), pas une shadow map. Flaque
   rectangulaire cisaillée qui s'allonge au couchant, poussière froxel
   en dalle, N fenêtres découpées simultanément, coût ~zéro. La
   direction UBO porte la NORMALE autorée de la fenêtre (marqueur
   w = −3) ; le gate d'orientation (soleil du bon côté du mur) vit dans
   le shader via ce couple normale/soleil. Limite assumée : le cadre
   découpe, pas le mobilier devant la fenêtre (ça, c'est la key shadow).
2. **Key shadows en atlas ×4** (2026-07-24, §5 B6 — était 1) : les 4
   lumières `shadowMode = "key"` les mieux scorées, une tuile 1024²
   chacune dans un atlas 2048² ; le slot voyage dans
   `LightsUbo.windowInfo.z` (le match par position est retiré), un
   caster UBO par tuile. L'ombre géométrique exacte (mobilier, lustre),
   intérieur comme extérieur — c'est la donnée qui décide.
3. **La GI** : occlusion ambiante directionnelle à l'échelle voxel ; et
   `shadowMode = "rcOnly"` route une lumière ENTIÈREMENT par le champ —
   pénombres molles gratuites (G7b), parfait pour bougies et ambiances ;
   la lumière quitte le direct ET les froxels, son blob GI porte tout.

**La politique est une donnée par lumière** (`LightForm.shadowMode`,
2026-07-24) : `""`/`"none"` = direct sans ombre (l'héritage
`castsShadow = true` vaut `"key"`), `"key"`, `"rcOnly"`. L'auteur — ou
un mod (§5) — arbitre le compromis coût/qualité lumière par lumière.

## 4. Trajectoire

1. ✅ Budget 24 + fenêtres-projecteurs (A) + `shadowMode` (C) —
   2026-07-24.
2. ✅ Atlas de 4 key shadows (B) — chantier CLUSTERED, brique B6.
3. ✅ Clustered forward sur la grille de froxels — chantier §5,
   **construit le 2026-07-24** (scène cible : la nuit aux torches).
4. Deferred — non, voir §2.

## 5. Chantier CLUSTERED — le plan (2026-07-24)

> Scène cible : la nuit aux torches, proches ET lointaines. Quatre
> défauts du forward actuel : le coût `pixels × slots` ; la sélection
> « 24 plus proches caméra » (pop-in des torches lointaines visibles,
> aucun frustum) ; le sol sun-only en direct (près d'une torche,
> l'illumination du sol vient du splat RC — doux, fenêtre ~32 m, pas de
> flaque nette) ; le saignement à travers les murs dès qu'il y a plus
> d'une key shadow à la fois. Chaque brique est livrable seule ; les
> chiffres F6 se consignent ici au fil de l'eau.

### 5.1 Le contrat RC × clustered

La RC reste justifiée, et devient plus propre : le clustered apporte du
DIRECT scalable ; il ne fournit rien de ce que la RC fait — occlusion
directionnelle de l'ambiante, multi-bounce (G7a), pénombres `rcOnly`
(G7b), fondu vers l'ambiante classique au loin. RC = indirect,
clustered = direct, froxels = le milieu.

Le point de friction est le **splat des lumières** (`rc_inject.comp`,
falloff aligné sur locallights.glsl) : il porte à la fois le « blob
direct » — ce qui éclaire le sol aujourd'hui — et la source du rebond.
Quand le direct touchera toutes les surfaces (B4) : le splat des
lumières normales est réduit à un rôle de **rebond** (facteur
albédo-like, knob catégorie GI, défaut ~0.35) ; les lumières `rcOnly`
gardent le splat plein (leur design) ; re-tuning au banc nuit, RC
ON/OFF en A/B. **EXTÉRIEUR seulement** (leçon 2026-07-24) : l'intérieur
ne dessine ni terrain ni herbe ni arbres — le clustered n'y ajoute
aucun receveur de direct, le facteur n'a rien à compenser et ne faisait
qu'éteindre la lueur tunée des fenêtres. La RC garde ses 24 lumières :
elle reçoit les 24 plus PROCHES (sa fenêtre fine fait ~32 m), pas les
64 du nouveau budget.

### 5.2 Les ombres face à 64 lumières

La politique par donnée (`shadowMode`, §3) reste LA réponse — le
clustered y est orthogonal. Ce qui change : l'atlas de key shadows ×4
(B6) ; les torches lointaines n'ont JAMAIS d'ombre (leur lisibilité =
halo froxel + bloom) ; bougies/ambiances restent `rcOnly`. La lune
comme directionnelle CSM de nuit (pure donnée, même mécanisme) est un
follow-up hors chantier.

### 5.3 Design acté

- **Grille de clusters = la grille froxel sous-échantillonnée ×8 en
  XY** : 16×9×64 (~9,2 k clusters), tranches Z IDENTIQUES aux froxels
  (même formule log, factorisée en include partagé) — un froxel
  retrouve son cluster par `xy/8`, même z.
- **Liste par cluster en SSBO std430** : `uint count + uint idx[31]`
  (128 o/cluster ≈ 1,2 Mo). Culling conservateur en sphère contre
  l'AABB monde du cluster (spots et fenêtres-projecteurs en sphère).
  **Leçon capacité (2026-07-24)** : à 16 slots, le hall intérieur
  (~18 sphères de lumière qui recouvrent les cellules centrales)
  débordait et éjectait les lumières les plus loin de la caméra — les
  fenêtres du fond perdaient leur flaque. 32 slots couvrent le pire
  empilement actuel ; en débordement, les plus proches de la caméra
  gagnent (l'ordre UBO).
- **Budget 24 → 64** : les tableaux du LightsUbo redimensionnés en
  lockstep (locallights.glsl, froxel_inject.comp, miroir
  LandscapeRenderer) — changement de layout coordonné, FrameUniforms
  intouché.
- **Fallback honnête** : clustered OFF (caps compute absentes ou
  checkbox « Clustered lights ») → boucle actuelle, count clampé à 24.
  `clusterInfo` appendu au FrameUbo (x = actif, yzw = params grille).
- Flicker CPU inchangé ; le match key-shadow migre du « distance de
  position < 0.05 » vers un index de slot porté par le LightsUbo (B6).

### 5.4 Briques — TOUTES CONSTRUITES (2026-07-24)

- ✅ **B0** — ce plan dans les docs (+ MEADOWS-PLAN) ; banc
  nuit-torches : commande console `torchbench <N>` (spirale dorée de
  torches, terrain +1,6 m, minuit forcé ; `torchbench 0` nettoie).
  **Baseline F6 : à relever par le dev au banc** (mainPass, froxels,
  RC, clusterCull) — consigner ici.
- ✅ **B1** — sélection frustum + importance (`collectLights` : cull
  sphère-frustum via `render::Frustum` réutilisé + `intersectsSphere`
  ajouté, score `intensity/(1+distSq)`, liste finale par distance,
  `stable_sort` déterministe) ; la RC reçoit les 24 plus proches.
  Hystérésis = knob futur si popping temporel observé.
- ✅ **B2** — `LightClusters.{hpp,cpp}` + `cluster_cull.comp` : 1
  thread/cluster, AABB monde de la cellule (4 coins × 2 tranches + le
  centre de la face lointaine — les tranches sont des coquilles de
  distance caméra, la face bombe), test sphère conservateur (spots et
  fenêtres en sphère), SSBO `count + idx[15]` (~590 Ko) ; budget 24→64
  en lockstep (locallights, froxel_inject, miroir C++).
- ✅ **B3** — chemin clustered dans `locallights.glsl`
  (`shadeLocalLight` factorisé ; cellule depuis `gl_FragCoord` +
  `sliceCoord` distance-caméra) ; checkbox « Clustered lights » +
  `LandscapeTuningForm.clusteredLights` — **défaut ON (2026-07-24)**.
  Legacy clampé à 24 — la passe reflet (flag OFF dans son FrameUbo)
  dégrade aux 24 plus proches.
- ✅ **B4** — terrain/herbe/arbres incluent locallights.glsl, gaté
  `uClusterInfo.x` (herbe : normale de brin + root AO, pas de bounce ;
  arbres : troncs et canopées) ; splat RC des lumières normales ×
  « Light splat bounce » (0.35, `rcOnly` exempté, appliqué CPU au
  remplissage des blobs). Solde le reliquat V2 de VOLUMETRIC.md.
  **Passe de réglage dev au banc : à faire** (RC ON/OFF en A/B).
- ✅ **B5** — `froxel_inject` lit la liste de sa cellule (xy
  sous-échantillonné, même z) — coût froxel plat quand le budget monte.
- ✅ **B6** — atlas de key shadows ×4 (cible 2048² = 2×2 tuiles 1024²,
  sélection par le même score d'importance, un caster UBO/groupe par
  tuile, viewport par tuile dans une passe ; slot dans
  `LightsUbo.windowInfo.z`, bornes resserrées d'un texel pour la PCF).
  Les champs FrameUbo single-light (`keyShadowViewProj/Info`) sont
  retirés du chemin, laissés en place (règle append-only).

Validation faite : build propre + 522 tests headless verts ; runs
Vulkan/MoltenVK clustered OFF et ON (overlay render-tuning temporaire),
0 erreur de validation. **Reste au dev : le visuel au banc** —
`torchbench 64` de nuit, A/B clustered, flaques au sol vs blob RC,
chiffres F6 à consigner ici, et l'orientation des tuiles d'atlas à
contrôler dans l'intérieur (TableSpot).

### 5.5 Vérification & hors scope

A/B : clustered OFF vs ON à ≤24 lumières → image identique (même math,
culling conservateur) ; extérieur jour clustered OFF → byte-identical.
Visuel nuit : flaques nettes au sol + blob RC en rebond (pas de double
éclairage), torches lointaines sans pop-in, pas de saignement à travers
les murs (B6), intérieur bougies inchangé. Hors scope, notés : LOD de
lumière (loin = emissive + bloom + halo froxel, zéro slot direct —
étage naturel de `shadowMode`), spéculaire stylisé des locales + cubemap
d'ambiance, lune-CSM, atlas avec cache si >4 key shadows un jour.
