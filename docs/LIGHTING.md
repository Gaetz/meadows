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

**Budget : 24** (2026-07-24, était 16), les N plus proches de la caméra
(`SceneSubmit`), une liste unique pour trois consommateurs : le direct
(LightsUbo), les blobs GI (RC), les froxels. Modèle de coût du forward :
`coût ≈ pixels × slots` — chaque slot se paie sur tout l'écran, même
vide. 24-32 est la zone de confort ; au-delà, la réponse n'est pas un
plus grand tableau :

- **Cible : clustered forward (Forward+)** — un compute assigne les
  lumières aux cellules d'une grille de frustum, chaque pixel ne boucle
  que sur sa cellule (2-4 lumières typiques) → des centaines de lumières.
  Synergie décisive ici : **la grille de froxels du fog (V4) EST une
  grille de clusters** — le même culling servirait fog et surfaces.
  À déclencher quand le F6 montre la boucle lumières dans le mainPass ou
  quand un intérieur dense le réclame (la mention « Clustered P1 » de
  MEADOWS-PLAN §B pointe désormais ici).
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
2. Atlas de 4 key shadows (B) — tiré dans le chantier CLUSTERED (§5,
   brique B6) : la nuit aux torches près des murs le réclame.
3. Clustered forward sur la grille de froxels — **déclenché 2026-07-24**
   (scène cible : la nuit aux torches proches et lointaines) ; le plan
   est le §5.
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
ON/OFF en A/B. La RC garde ses 24 lumières : elle reçoit les 24 plus
PROCHES (sa fenêtre fine fait ~32 m), pas les 64 du nouveau budget.

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
- **Liste par cluster en SSBO std430** : `uint count + uint idx[15]`
  (64 o/cluster ≈ 590 Ko). Culling conservateur sphère/cône contre
  l'AABB view-space du cluster (fenêtre-projecteur traitée en sphère).
- **Budget 24 → 64** : les tableaux du LightsUbo redimensionnés en
  lockstep (locallights.glsl, froxel_inject.comp, miroir
  LandscapeRenderer) — changement de layout coordonné, FrameUniforms
  intouché.
- **Fallback honnête** : clustered OFF (caps compute absentes ou
  checkbox « Clustered lights ») → boucle actuelle, count clampé à 24.
  `clusterInfo` appendu au FrameUbo (x = actif, yzw = params grille).
- Flicker CPU inchangé ; le match key-shadow migre du « distance de
  position < 0.05 » vers un index de slot porté par le LightsUbo (B6).

### 5.4 Briques

- **B0** — ce plan dans les docs (+ MEADOWS-PLAN) ; banc nuit-torches :
  commande console de spawn de N torches (flicker, rayon ~8 m) autour du
  joueur + heure forcée nuit ; baseline F6 (mainPass, froxels, RC).
- **B1** — sélection frustum + importance (CPU, indépendant) :
  `collectLights` cull sphère-frustum + score `intensity/(1+distSq)`,
  `stable_sort` conservé ; la RC reçoit les 24 plus proches. Hystérésis
  = knob futur si popping temporel observé, pas construit d'avance.
- **B2** — `LightClusters.{hpp,cpp}` + `cluster_cull.comp` (modèles
  GpuOcclusion/chunk_cull) : 1 thread/cluster, boucle 64 slots, écrit
  count+indices ; budget 24 → 64 dans le même mouvement.
- **B3** — `locallights.glsl` chemin clustered (cluster depuis
  `gl_FragCoord` + tranche log, boucle sur la liste) ; toggle panneau
  render + champ tuning (Save).
- **B4** — terrain/herbe/arbres incluent locallights.glsl, **gaté au
  chemin clustered uniquement** (herbe : normale de brin du modèle wrap,
  pas de bounce intérieur) ; MÊME brique : le facteur rebond du splat RC
  (§5.1) + passe de réglage dev au banc. Solde le reliquat V2 de
  VOLUMETRIC.md.
- **B5** — `froxel_inject` lit la liste du cluster (`xy/8`, même z) —
  le coût froxel reste plat quand le budget monte.
- **B6** — atlas de key shadows ×4 (tuiles 1024², les 4 `"key"` les
  mieux scorées, échantillonnage par index de slot).

### 5.5 Vérification & hors scope

A/B : clustered OFF vs ON à ≤24 lumières → image identique (même math,
culling conservateur) ; extérieur jour clustered OFF → byte-identical.
Visuel nuit : flaques nettes au sol + blob RC en rebond (pas de double
éclairage), torches lointaines sans pop-in, pas de saignement à travers
les murs (B6), intérieur bougies inchangé. Hors scope, notés : LOD de
lumière (loin = emissive + bloom + halo froxel, zéro slot direct —
étage naturel de `shadowMode`), spéculaire stylisé des locales + cubemap
d'ambiance, lune-CSM, atlas avec cache si >4 key shadows un jour.
