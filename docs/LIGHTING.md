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
2. **1 key shadow** (1024², perspective) : la lumière `shadowMode =
   "key"` la plus proche — l'ombre géométrique exacte (mobilier,
   lustre). Extension possible : un atlas de 4 tuiles (~0.4 ms) quand le
   contenu réclamera plusieurs ombres nettes — non construit.
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
2. Atlas de 4 key shadows (B) — quand le mobilier réclamera plusieurs
   ombres nettes simultanées.
3. Clustered forward sur la grille de froxels — quand le nombre de
   lumières ou le F6 le réclame ; enterre définitivement la question du
   nombre.
4. Deferred — non, voir §2.
