# Radiance Cascades — GI 3D world-space (chantier RC)

> Chantier décidé le 2026-07-11 (dev) : une GI dynamique par **radiance
> cascades** (Sannikov, JCGT [WIP] — « Radiance Cascades: A Novel Approach
> to Calculating Global Illumination »), en grille 3D world-space,
> construite **en parallèle** du lighting actuel (technique switchable —
> intéressant pour le modding à terme). Version QUALITÉ d'abord ; la
> descente perf est faite PAR LE DEV via les knobs UI (cible progressive
> 3 ms puis 1-2 ms au F6 HUD). À lire avant de toucher
> `engine/render/landscape/RadianceCascades.*`, `gi.glsl`, ou les shaders
> `rc_*.comp`.

---

## 1. La théorie en dix lignes

- **Intervalle de radiance** `L[a,b](p,ω)` : la lumière arrivant en p
  depuis le segment [a,b] le long de ω, avec sa transparence β. Deux
  intervalles adjacents **fusionnent** : `L[a,c] = L[a,b] + β[a,b]·L[b,c]`.
- **Condition de pénombre** : résoudre une pénombre exige un pas LINÉAIRE
  ∝ D et un pas ANGULAIRE ∝ 1/D. D'où la hiérarchie : la cascade i espace
  ses probes ×2ⁱ, résout 4ⁱ× plus de directions, et couvre l'intervalle
  [~2ⁱ, ~2ⁱ⁺¹]·t₀.
- En 3D, la cascade i coûte **la moitié** de la cascade i-1 (probes /8,
  directions ×4) → toute la hiérarchie ≤ 2× la cascade 0. « Une infinité
  de rayons en temps fini » : chaque niveau DOUBLE le nombre effectif de
  rayons pour un coût divisé par deux.
- Les intervalles s'interpolent **linéairement sans leaks par
  construction** (chaque cascade est sous sa fréquence de Nyquist) — pas
  de bilateral/disocclusion à la DDGI.
- Reconstruction : merge N→0 (par texel de la cascade i : moyenne des 4
  texels angulaires des 4 probes parentes de i+1, fusion β), puis
  l'irradiance = somme cosinus des quelques directions de la cascade 0.

## 2. Architecture Meadows

Trois étages, tous centrés caméra, tous re-calculés chaque frame
(single-shot : zéro lag, zéro ghosting — propriété du papier) :

```
scène → [INJECTION] clipmap voxel (2 niveaux 64³)
      → [BUILD]     cascades 3D (5 niveaux, raymarch du clipmap)
      → [MERGE N→0] cascade 0 = radiance complète 8 directions
      → [APPLY]     gi.glsl : le terme ambiant des 5 shaders de surface
```

Le far-field (> portée des cascades) reste l'existant : ambiante ciel ×
terrain light map — exactement la complémentarité AO-près / env-map-loin
que le papier théorise. Contact shadows et vertex AO restent aussi (haute
fréquence sous la taille de voxel).

### 2.1 Clipmap voxel (le volume que les intervalles raymarchent)

- 2 niveaux **64³ RGBA16F** : voxel 0,5 m (span 32 m, intérieurs/détail)
  et voxel 2 m (span 128 m, mid-field). rgb = radiance directe injectée,
  a = occupation (0/1, soft aux surfaces).
- Alignés sur la grille voxel (snap au voxel, pas à la caméra — stabilité).
- **Injection compute chaque frame** (`rc_inject.comp`, un thread/voxel) :
  - **terrain** : hauteur/normale/matériau ANALYTIQUES (TerrainNoise,
    même chemin que TerrainLightMap) ; voxel sous la hauteur → solide
    (l'occupation souterraine bloque correctement les intervalles vers
    le bas) ; voxel de surface → albedo (palette des poids de matériau)
    × (soleil CSM (uShadowMap, pattern du volumétrique) × N·L + ambiante
    ciel affaiblie) ;
  - **props/kits/PNJ (G3)** : SSBO d'AABBs depuis le snapshot extract ;
    v1 = boîtes pleines, albedo moyen — un arbre occlut comme sa boîte,
    assumé stylisé ; triangles réels = plus tard ;
  - **lumières locales (G3)** : les 16 lumières de la frame splattées
    analytiquement (radiance += couleur×falloff sur les voxels de
    surface du rayon).

### 2.2 Cascades (5 niveaux par défaut, knob)

| i | probes | pas | dirs | intervalle (×interval0, défaut 1 m) |
|---|--------|-----|------|--------------------------------------|
| 0 | 64³ | 0,5 m | 8   | [0, 1) + [1, 3) *(t₀ inclut le near)* |
| 1 | 32³ | 1 m   | 32  | [3, 7) |
| 2 | 16³ | 2 m   | 128 | [7, 15) |
| 3 | 8³  | 4 m   | 512 | [15, 31) |
| 4 | 4³  | 8 m   | 2048| [31, 63) |

- **Directions** : octaédral plein-sphère, `dirs(i) = 8·4ⁱ`.
- **Layouts mémoire** (~33 Mo total RGBA16F) :
  - **cascade 0** (la cible d'apply) : dir-major — texture 3D
    `64×64×(64·8)` ; chaque « slab » z = une direction → **trilinéaire
    hardware** entre probes d'une même direction (clamp demi-texel aux
    bords de slab).
  - **cascades 1+** : dir-tile — texture 3D `(P·tx)×(P·ty)×P` (la grille
    de directions tuilée en XY) ; le merge fait son interpolation probe
    MANUELLEMENT de toute façon (pondération β), pas besoin du filtre HW.
    (Un layout dir-major dépasserait GL_MAX_3D_TEXTURE_SIZE dès c3.)
- **Build** (`rc_build.comp`, un thread/texel) : raymarch du clipmap
  (fin pour c0-c1, grossier pour c2+), pas = taille de voxel, sortie
  rgb+β. L'**extension d'intervalle** du papier (raymarcher court puis
  doubler par shift+merge, O(1)) = l'optim majeure, brique G7.
- **Merge** (`rc_merge.comp`) : de N-1 vers 0 ; par texel : 4 probes
  parentes (bilinéaire manuel) × moyenne des 4 texels angulaires enfants,
  `L += β·L_parent`.

### 2.3 Apply — gi.glsl et le switch

- `enum class GiTechnique : u32 { Classic, RadianceCascades }` (règle
  enum) — combo UI, `uGiInfo` dans le FrameUbo (APPEND-only + lockstep
  common.glsl + purge *.dir).
- Dans terrain/mesh/skinned/grass/water.frag, le terme ambiant devient
  UN point de branche uniforme :
  - `Classic` → chemin actuel **byte-identique** ;
  - `RadianceCascades` → `giIrradiance(worldPos, normal)` : 8 fetches
    trilinéaires de la cascade 0 fusionnée × poids cosinus, fondu vers
    l'ambiante classique aux bords du clipmap et au-delà de la portée.
- La composante soleil directe/CSM/spéculaire ne change JAMAIS — RC ne
  remplace que l'INDIRECT (l'ambiante).

### 2.4 UI « Global illumination » (workflow dev : la perf se règle ICI)

Technique (combo) · tailles/voxels des 2 clips (recreate keyé sur valeurs
appliquées, pattern reflectionScale) · nb de cascades · dirs c0 ·
interval0 · intensité · portée/fondu du fallback · cadence (1/1, 1/2
frame) · vues debug (volume raymarché, slabs cascade 0) · **coût GPU par
passe** (scopes GpuProbe : rcInject, rcBuild, rcMerge) affiché dans la
catégorie. Chaque paramètre naît DANS l'UI, jamais en constante.

## 3. Briques

- **G0 ✅ (`9f7d535`)** — RHI : textures 3D (`TextureDesc.depth`,
  `caps().volumeTextures`, WRAP_R, `addressW`, image binds LAYERED).
- **G1 ✅** — ce document.
- **G2** — clipmap + injection terrain/soleil + vue debug (raymarch du
  volume dans le Debug buffer) + catégorie UI + scopes GpuProbe.
- **G3** — injection props/PNJ (AABBs) + lumières locales.
- **G4** — build des cascades ; **G5** — merge N→0 + vue debug cascades.
- **G6 ✅ validé dev 2026-07-11** — gi.glsl + switch GiTechnique dans
  terrain/mesh/skinned/grass (l'eau attend) ; Classic par défaut,
  byte-identique. Retours dev intégrés le jour même :
  1. *trop fort* → splat des lumières aligné sur l'inverse-square fenêtré
     de locallights.glsl + intensité défaut 0,7 ;
  2. *intérieurs* → actifs (tuile terrain = placeholder « no terrain »
     en intérieur, les boîtes de kit + lumières portent la pièce ;
     re-bascule automatique en re-sortant) ;
  3. *ramp BotW* → l'irradiance GI est POSTERISÉE en fin de calcul
     (3 bandes plates relatives à l'ambiante classique, teinte préservée,
     gatée par le blend stylisé uAmbientColor.w).
  **Clarification (question dev)** : dans cette intégration, RC ne
  remplace que l'INDIRECT (l'ambiante) — les ombres du soleil restent la
  CSM. Ce que RC « ombre » = l'occlusion directionnelle de l'indirect
  (d'où la lecture « ambient occlusion ») + le rebond occlus des
  lumières. Pour de VRAIES ombres RC (pénombres par construction), il
  faudrait router des lumières EXCLUSIVEMENT par RC (le modèle PoE2) —
  candidate G7 ci-dessous.
- **G7a/G7b ✅ (2026-07-11)** — multi-bounce par feedback temporel (le
  inject relit la cascade 0 fusionnée de la frame précédente, knob
  « Bounce feedback » 0.5) et **« Lights via RC only »** (le direct des
  lumières coupé — pénombres par le volume). Baseline dev : intensity
  1.0, skyFactor 1.0. Itérations rampe du jour : domaine asymétrique
  unifié `[moyenne−dimRange, max]`, blobs émetteurs des lumières
  (« Light emitter boost »), offset normal à l'apply.
- **G7c (restant)** — extension d'intervalle (l'optim majeure du build,
  à faire pendant/avant la **passe de réglage dev** → 3 ms → 1-2 ms) ;
  triangles fins en intérieur si les boîtes de kit leakent.
- **G8** (si mesuré nécessaire) — accélération spatiale : index spatial
  CPU partagé moteur/gameplay et/ou voxels sparse.

## 4. Risques assumés

- AABB v1 = occlusion cubique des props (stylisé : acceptable, trianglé
  plus tard). Murs plus fins que 0,5 m : leaks possibles en intérieur —
  à évaluer G6 (mitigations : clip fin, injection épaissie).
- Portée ~63 m (×interval0) : au-delà, fondu vers l'ambiante classique.
- Coût inconnu avant mesure (accepté — qualité d'abord) ; les leviers de
  descente sont TOUS dans l'UI.

## 5. Références

- Papier : `Downloads/RadianceCascades.pdf` (Sannikov, JCGT WIP) — lu et
  digéré ici ; §2.3.3 extension d'intervalle, §2.5.2 scaling 3D, §4.3
  Radiance 3d (le précédent world-space voxelisé).
- Communauté : GM Shaders « Radiance Cascades » (Xor + Sannikov),
  tmpvar/poc/radiance-cascades, shadertoys fad `mtlBzX` (le canon 2D),
  `lXByRh` (référence du dev), `X3XfRM` (3D).
