# FIRE-RENDER — le rendu du feu et son éclairage

> Référence de conception (demande dev 2026-09-27, après E3.b). Recherche
> compilée depuis : Levesque (Far Cry 2 fire postmortem), Cyanilux et
> Febucci (flame shader breakdowns), G. Silva (reconstruction des VFX de
> BotW), Lozar (flipbooks à vecteurs de mouvement), DeepSpaceBanana / W. Zhu
> (burn shaders), guide Unreal « Particle Lights », GPU Gems 2 ch. 19 et
> GPU Gems 3 ch. 23, Ghost of Tsushima VFX. Aucune conférence publique ne
> documente l'éclairage du feu de BotW/TotK, Firewatch ou RDR2 : leurs
> approches sont inférées de reconstructions, pas de slides officielles.
> Les briques ci-dessous se valident visuellement, chacune derrière une
> bascule A/B (règle projet), avant tout commit.

## 1. Ce que le moteur a déjà (le point de départ)

| Brique existante | Où | Rôle pour le feu |
|---|---|---|
| Champ de feu 2 m (chaleur / combustible / état) | `engine/terrain/FireField`, `world/spirit/SpiritFire` | la vérité CPU, 10 Hz |
| Masque de scorch R16F, 1 texel / cellule, unité 10 | `engine/render/landscape/FireScorchMap`, `firescorch.glsl` | terrain charbonné, herbe brûlée |
| Émetteurs CPU billboards alpha/additif, 24 flammes « transplantées » | `engine/fx/Particles`, `LandscapeScene::updateSpiritFire` | les flammes v0 |
| Forward+ clusterisé, lumières ponctuelles du snapshot | `RenderSnapshot.lights`, `locallights.glsl` | l'éclairage du feu |
| Cascades de radiance (GI), bloom HDR | `gi.glsl`, tonemap | le rebond orangé, la lueur |
| Volume de bruit 3D (brume volumétrique) | `noiseVolumeGroup` | bruit des flammes, du liseré, de la distorsion |
| Vent : force scalaire seulement, direction = E4 | `windInfo` | fumée, braises, inclinaison des flammes |

## 2. Les quatre techniques retenues (rapport look / coût, stylisé)

1. **Quads de flamme procéduraux** face caméra : bruit qui défile + rampe
   de forme + **bandes de couleur posterisées** + érosion alpha + fondu de
   profondeur (soft particles). Deux fetchs de texture, pas d'atlas, une
   graine par particule donc jamais deux flammes identiques. C'est
   exactement le look BotW / Genshin (bandes plates, bord dur érodé, pas de
   fumée réaliste). Les émetteurs CPU restent ; seul le shader de la
   particule change. **Écartés** : les flipbooks à vecteurs de mouvement
   (sim FumeFX/Houdini hors ligne + atlas non compressé, du réalisme qu'on
   ne veut pas) et le feu volumétrique ray-marché (128+ pas par pixel).
2. **Front de brûlure continu dans le shader de terrain**, piloté par le
   masque de feu (bilinéaire + érosion par bruit) : la zone charbonnée ET
   le **liseré de braises** en une seule ligne continue, au lieu de décals
   par cellule. Far Cry 2 (« grille 2D projetée sur le terrain ») et les
   systèmes de propagation RealtimeVFX font pareil (les matériaux lisent
   une texture de simulation). Le plus gros gain visuel après les flammes,
   coût quasi nul — notre `FireScorchMap` est déjà cette texture, il lui
   manque le canal chaleur.
3. **Lumières de feu agrégées et scintillantes + émissif → GI et bloom** :
   les cellules brûlantes regroupées en 8-32 lumières ponctuelles ; les
   quads et le liseré marqués émissifs pour que les cascades de radiance
   portent le rebond orangé sur la prairie et les personnages, et que le
   bloom morde sur l'émissif HDR. Règle Unreal : « très peu de grandes
   lumières, un plus grand nombre de petites » ; le coût = lumières ×
   pixels couverts.
4. **Garniture** : braises/étincelles (additif, minuscules, soulevées par le
   vent), un billboard de fumée doux pour ~4 cellules dérivant avec le vent
   global (Ghost of Tsushima : le vent dans chaque effet, cendres sur les
   zones brûlées), et une passe de **distorsion de chaleur** écran (GPU
   Gems 2 ch. 19 : perturber l'UV écran par une normale/bruit animé,
   masquer l'avant-plan par la profondeur pour éviter les fuites).

## 3. Recette du quad de flamme

Vertex : billboard autour de l'ancre de la cellule ; effiler le haut
(`x *= 1 - 0.6 * uv.y²`) en goutte ; par instance `seed`, `age`, `heat`.

```glsl
vec2 uv = vUv;                                   // 0..1, y vers le haut
float n1 = tex(noise, uv * vec2(1, 2) * scale + vec2(seed, -t * 1.2)).r;
float n2 = tex(noise, uv * scale * 0.5 + vec2(seed * 3, -t * 0.7)).r;
float n  = 0.65 * n1 + 0.35 * n2;
uv.y += (n - 0.5) * distort * uv.y;              // la distorsion croît vers la pointe
float shape = tex(gradient, uv).r;               // base claire, pointe sombre (ou 2 ellipses)
float v = shape - (1.0 - heat) * 0.3;            // la chaleur rétrécit la flamme
float a  = step(n - 0.0, v);                     // bande externe -> alpha (bord dur BotW)
float b1 = step(n - 0.2, v);                     // bande médiane
float b2 = step(n - 0.4, v);                     // cœur
vec3 col = mix(outerCol, midCol, b1);
col = mix(col, coreCol * 3.0 /* HDR */, b2);
float soft = saturate((sceneDepth - fragDepth) / softDist);   // soft particles
float fade = 1.0 - smoothstep(0.7, 1.0, age);   // éroder : monter le seuil, pas baisser l'alpha
a *= soft * step(fade, a);
```

Corps en alpha, plus un quad de cœur additif si l'on veut que le bloom
morde. Rampe par chaleur : cœur jaune-blanc → orange → rouge profond en
bordure, moignon de fumée rouge sombre/noir à la pointe. Variante Godot
« Stylized Flame » (bandes depuis `dot(N, V)` sur une petite sphère) pour
les 3-5 flammes héros les plus proches de la caméra, lisibles sous tout
angle.

## 4. Recette de l'éclairage

- **Agrégation** : grille 2 m → tuiles de 8 m (16 cellules). Par tuile qui
  brûle : une lumière au barycentre pondéré par la chaleur,
  `rayon = 4 m + 2 m × √(cellules brûlantes)`, `intensité = k × Σ chaleur`.
  Trier par couverture écran, garder les N plus proches (N = 16 par défaut,
  32 en haute qualité), fondre le reste en 1-2 grandes lumières faibles.
  Option : 1 petite lumière par flamme héros à moins de 15 m.
- **Scintillement** (déterministe, graine `s` par lumière, bruit de valeur
  `vn`) :
  `I = I0 × (1 + 0.25 × (vn(t×7 + s) − 0.5) + 0.10 × (vn(t×23 + s×2) − 0.5))`,
  position `pos += 0.15 m × (vn3(t×5 + s) − 0.5)`, rayon ±10 %. Deux
  octaves évitent l'aspect sinusoïdal.
- **Rampe de température par chaleur** : 1,0 → (1.0, 0.75, 0.45) ; 0,5 →
  (1.0, 0.50, 0.15) ; mourante → (0.9, 0.25, 0.05). La même rampe sur le
  liseré de braises.
- **GI / bloom** : quads et liseré émissifs pour que les cascades portent
  le rebond orangé ; bloom piloté par l'émissif HDR seul (cœur ×3, liseré
  ×2). Pas de cookies : le bruit d'intensité fait le même effet à l'œil.
- Far Cry 2 : budget fixe d'émetteurs et de lumières, téléportés de
  derrière la caméra vers l'avant, plus denses près du joueur (ce que
  `updateSpiritFire` fait déjà pour les 24 flammes).

## 5. Liseré de braises, sol brûlé, herbe

Masque de feu **RG**, 1 texel / 2 m : R = progression de brûlure (0 intact →
1 brûlé, notre scorch), **G = chaleur** (à ajouter). Terrain :

```glsl
vec2 fm = tex(fireMask, worldXZ / 2.0 + 0.5 / maskSize).rg;
float n = tex(noise, worldXZ * 0.35 + vec2(0, t * 0.05)).r;
float e = fm.r + (n - 0.5) * 0.35;               // front érodé
float charred = smoothstep(0.50, 0.58, e);
float rim     = smoothstep(0.38, 0.47, e) - smoothstep(0.50, 0.60, e); // bande juste hors charbon
albedo   = mix(albedo, vec3(0.06, 0.05, 0.04), charred);
emissive += rim * emberRamp(fm.g) * (2.0 + 0.6 * sin(t * 9 + n * 30)) * fm.g; // braise qui respire
```

Décaler la lueur (0,45) sous le charbon (0,5) est la recette
DeepSpaceBanana / Zhu : le charbon mange la lueur, le liseré reste une
bande fine sur le front qui brûle et meurt avec la chaleur. Herbe : le même
masque ; `hauteur *= 1 − charred × 0.8`, pointe qui vire au noir avec une
teinte braise sur `rim`, brins supprimés quand `charred > 0.9` (Far Cry 2 :
« la cellule qui brûle prévient la végétation »). Fumée : 1 billboard alpha
doux pour 4 cellules brûlantes, 3-5 s de vie, monte + vent global, même
érosion que les flammes avec une rampe grise. Distorsion de chaleur :
dessiner les quads de chaleur dans un tampon R8 basse résolution, puis en
post `uv += (noise2D(uv × 8 + t) − 0.5) × 0.01 × heat`, masqué par la
profondeur.

## 6. Budgets et LOD

- Le coût, c'est l'overdraw, pas le nombre de particules. Cibles : 400-800
  quads de flamme, 200-500 braises, 50-100 sprites de fumée à l'écran ; si
  un front remplit l'écran, particules en demi-résolution puis composées
  (GPU Gems 3 ch. 23).
- LOD par distance : < 30 m : 3-5 quads / cellule + braises + lumière héros ;
  30-80 m : 1 quad plus grand par 2×2 cellules ; > 80 m : pas de quads, le
  liseré émissif seul, 1 colonne de fumée par tuile de 8 m et la lumière
  agrégée. « Hair transplant » de Levesque : quand les émetteurs manquent,
  agrandir les survivants plutôt qu'en créer.
- Lumières : 8-32 lumières de feu dans la liste forward+, rayon borné pour
  garder l'occupation des clusters basse.

## 7. Découpage proposé en briques (à trancher par le dev)

| Brique | Contenu | Dépend de | Bascule A/B |
|---|---|---|---|
| **F1 — liseré de braises** | canal chaleur dans `FireScorchMap` (RG16F), front érodé + `rim` émissif dans terrain.frag et grass.frag, herbe qui se rabat avant de disparaître | rien | `uFireScorchInfo` ou un knob du panneau de rendu |
| **F2 — quads de flamme** | pipeline particule « flamme » (bruit + rampe + bandes + érosion + soft depth), rampe par chaleur, 3-5 quads/cellule près, 1 par 2×2 loin | F1 (même masque) | choix du ParticleForm |
| **F3 — lumières de feu** | agrégation 8 m → `RenderSnapshot.lights` (≤ 16/32), scintillement 2 octaves, rampe de température, émissif → cascades + bloom | F2 | knob « fire lights » |
| **F4 — garniture** | braises, fumée avec vent, distorsion de chaleur écran | E4 (direction du vent) | knobs |

Chaque brique = un ParticleForm / des champs `SpiritForm` (couleurs,
rayons, budgets) en data, jamais des constantes dans le shader (règle
« les boutons de perf vivent dans l'UI »).

## 8. Références

1. Levesque, « Far Cry: How the Fire Burns and Spreads » —
   https://jflevesque.com/2012/12/06/far-cry-how-the-fire-burns-and-spreads/
2. Cyanilux, « Fire/Flame Shader Breakdown » —
   https://www.cyanilux.com/tutorials/fire-shader-breakdown/
3. Febucci, « Unity Fire Shader: Procedural Flames » —
   https://blog.febucci.com/2019/05/fire-shader/
4. Silva, « Recreating the Zelda VFX in Unity » —
   https://www.gamedeveloper.com/art/recreating-the-zelda-vfx-in-unity
5. Lozar, « Frame Blending with Motion Vectors » —
   https://www.klemenlozar.com/frame-blending-with-motion-vectors/
6. DeepSpaceBanana, « Flowmapped Burn Shader » —
   https://deepspacebanana.github.io/blog/shader/art/unreal%20engine/Flowmapped-Burn-Shader
7. Zhu, « Interactive Burning Effects » —
   https://www.wentianzhu.com/tablog/unity-interactive-burning-effects
8. RealtimeVFX, « Fire propagation system » —
   https://realtimevfx.com/t/fire-propagation-system-shader-bp-vfx/5245
9. Unreal, « Particle Lights » —
   https://dev.epicgames.com/documentation/en-us/unreal-engine/particle-lights?application_version=4.27
10. GPU Gems 2 ch. 19 « Generic Refraction Simulation » —
    https://developer.nvidia.com/gpugems/gpugems2/part-ii-shading-lighting-and-shadows/chapter-19-generic-refraction-simulation
11. GPU Gems 3 ch. 23 « High-Speed, Off-Screen Particles » —
    https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-23-high-speed-screen-particles
12. Godot Shaders, « Stylized Flame » — https://godotshaders.com/shader/stylized-flame/
13. PlayStation Blog, Ghost of Tsushima VFX —
    https://blog.playstation.com/2021/01/12/how-stunning-visual-effects-bring-ghost-of-tsushima-to-life/
