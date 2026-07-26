# Chantier — Optimisation GPU du renderer landscape

> **PLAN (2026-07-10, nuit) — aucune mesure GPU n'existe encore.** Tout
> « gain attendu » ci-dessous est une HYPOTHÈSE (leçon audit §0, payée
> deux fois : un remède sans mesure est une hypothèse). La brique P0
> fabrique l'outil de mesure manquant ; **l'ordre final des briques P2+
> sera décidé par la table baseline de P0**, pas par ce document.
> Contexte d'entrée : sim CPU ≤ 2 ms — la frame est GPU/driver-bound.

## Contexte

Inventaire de frame vérifié (`game/scenes/LandscapeRenderer.cpp`,
`render()`) :

| # | Passe | Coût structurel connu |
|---|-------|------------------------|
| 1 | Cloud bake 512² (chaque frame) | 1 fullscreen 512² |
| 2 | Key-light shadow 1024² | intérieur seulement |
| 3 | Occlusion pluie 512² | pluie seulement |
| 4 | CSM 3 × 2048² Depth32F, re-rendu CHAQUE frame | terrain (cap 9 chunks) + végétation + meshes/PNJ ; le soleil est déjà quantifié (~0,4°/~8 s) |
| 5 | Réflexion planaire demi-res | 2ᵉ rendu scène (terrain + arbres low-LOD + ciel) dès que caméra > seaLevel en extérieur, même sans eau visible |
| 6 | Passe opaque HDR RGBA16F pleine res | terrain (rayon 15 ≈ 960 m), végétation (rayon 14, canopées 320 faces < 256 m), HERBE (rubans 7-tri, 0,15 m, ring 3, préfixe de densité métrique), meshes, PNJ skinnés (non instanciés), ciel, cumulonimbus (FBM 4 octaves/pixel), volumes d'eau, shafts, pluie (3000 streaks FIXES quel que soit rainIntensity) |
| 7 | Copies color+depth pleine res, Hi-Z compute + cull (fence P1), composite eau | |
| 8 | PostFx demi-res : bloom 5 niveaux, god rays 48 taps, volumétrique 20 pas (sample CSM), SSAO, contact shadows 12 pas, auto-expo 64² | **vérifié (`PostFx.cpp:309-378`) : god rays, volumétrique et SSAO tournent INCONDITIONNELLEMENT, même à intensité 0 et en intérieur** — les intensités s'appliquent dans `tonemap.frag` |
| 9 | Tonemap composite pleine res + UI | |

Instrumentation actuelle : `engine/core/FrameProbe.hpp` = horloges **CPU**
seulement (Release, log > 25 ms). **Aucun timer GPU dans `engine/rhi/`.**
Le motif à copier existe : les fences (`Device.hpp:84-86`,
`GlDeviceBase.cpp` — l'audit P1).

Résiduels connus : `terrain=133ms` au boot (warmup driver, accepté) ;
`terrain=14ms` épisodique en déplacement (stall d'upload LOD0, non
re-mesuré depuis P1b).

Tous les knobs structurels sont des constantes compile-time :
`TerrainSystem::kViewRadius=15`, `GrassSystem::kViewRadius=3` /
`kBladeSpacing=0.15`, `VegetationSystem::kViewRadius=14` /
`kHighDetailRadius=4`, `ShadowMapper::kResolution=2048` /
`kCascadeCount=3` ; les résolutions PostFx/réflexion/Hi-Z sont codées en
dur (demi-res). Les seuls knobs data (`LandscapeTuningForm`) sont des
INTENSITÉS.

## Règles du chantier (non négociables)

1. **MESURER D'ABORD.** Aucune brique P2+ ne se code avant que P0 ait
   donné le coût de la passe visée. Une passe < 0,5 ms = brique
   abandonnée sans état d'âme.
2. **Look-neutre ou togglé.** L'extérieur ne change JAMAIS sans A/B.
   Chaque brique déclare : byte-identique par défaut, ou derrière un
   toggle/slider.
3. **Tout passe par le RHI** — GL seulement dans `backends/gl`. Les
   timers GPU = extension du Device, comme les fences.
4. **Biais prototype** (§1 CLAUDE.md) : pas de framegraph, pas de pool
   sophistiqué. La chose la plus simple qui mesure/optimise.
5. FrameUbo : **APPEND-ONLY** en fin de struct, static_asserts +
   `common.glsl` en lockstep, purge `*.dir` (leçon payée).
6. Descopés restent descopés : TAA, LUT 3D, caustiques, GL 4.1 dégradé,
   impostors.

---

## Brique P0 — Timers GPU : l'outil avant tout (M)

> **FAITE (2026-07-10, nuit) — baseline dev à relever.** RHI :
> `insertTimestamp/timestampReady/destroyTimestamp` (GlDeviceBase,
> caps.timerQueries sur le 4.6) ; `engine/render/GpuProbe` (ring 4
> frames, jamais bloquant) ; scopes posés (cloudBake, keyShadow,
> rainOcc, shadows, reflection, mainPass + mainTerrain/mainVeg/
> mainGrass, copyHizWater, postfx + bloom/godrays/volumetric/ssao/
> contact/autoExpo, composite) ; HUD = section **« GPU perf [F6] »** du
> panneau de la scène Landscape (table GPU moy/max + colonne CPU,
> reset). Builds Debug + Release + 345 tests verts + smoke. **Prochaine
> étape : le dev relève la table baseline sur les 4 spots (protocole en
> fin de doc) — elle ordonne P1+.**

**But :** un budget ms/passe GPU, en live et loggé sur frame lente.
Zéro stall (le sync readback a déjà coûté 25 ms/frame une fois).

- **RHI** (miroir exact du motif fence) : `TimestampHandle` (Rhi.hpp),
  caps `timerQueries`, et sur Device :
  `insertTimestamp()` (glQueryCounter GL_TIMESTAMP),
  `timestampReady(handle, u64& nanos)` (poll GL_QUERY_RESULT_AVAILABLE,
  timeout 0, jamais bloquant ; consomme le handle quand prêt),
  `destroyTimestamp(handle)` (teardown). Single-use volontaire — une
  free-list de queries n'arrive QUE si un profil la réclame.
- **`engine/render/GpuProbe.{hpp,cpp}`** : le pendant GPU de FrameProbe,
  motif fence-découplé de GpuOcclusion. Ring de 4 frames en vol ; un
  slot n'est résolu que quand TOUS ses timestamps sont prêts ;
  back-pressure si le ring est plein (la frame courante n'instrumente
  pas). `Scope` RAII (`GpuProbe::Scope s { probe, device, "shadows" }`).
  Stats glissantes 120 frames (moyenne + max). No-op sans caps.
  Sémantique honnête (dans l'en-tête) : un timestamp GL mesure quand le
  GPU ATTEINT ce point du stream — sur une queue unique c'est un vrai
  temps de passe si le scope enveloppe l'enregistrement de la passe.
- **Scopes** dans `render()` : cloudBake, keyShadow, rainOcc, shadows,
  reflection, mainPass (+ sous-scopes mainTerrain/mainVeg/mainGrass —
  les trois suspects), copyHizWater, postfx (+ sous-scopes bloom/
  godrays/volumetric/ssao/contact/autoExpo — `PostFx::render()` prend un
  `GpuProbe*` optionnel), composite.
- **HUD** `drawPerfPanel()` : table `Passe | GPU moy | GPU max | CPU
  moy`, total GPU, fps, résolution offscreen ; reset fenêtre. FrameProbe
  garde une copie `lastEntries` (colonne CPU). Log
  `WARN "gpu frame spike"` > 25 ms avec breakdown, apparié au log CPU
  par index de frame.
- **Validation :** overhead on/off < 0,1 ms CPU ; somme des passes ≈
  total (l'écart = passes non instrumentées, c'est un résultat aussi) ;
  couper chaque toggle A/B existant fait tomber sa passe à ~0.
  **Livrable : la table BASELINE sur les 4 spots du protocole — c'est
  elle qui réordonne P1+.**

## Brique P1 — Knobs structurels en données + render scale (M)

APPEND `LandscapeTuningForm` (+ REFLECT + `landscape.toml`, défauts =
valeurs actuelles = byte-identique) :

| Champ (défaut) | Source | Changement runtime |
|---|---|---|
| `terrainViewRadius` (15, clamp 6-31) | TerrainSystem | live-sûr (ring ChunkStreamer ; 31 → 961 candidats < cap Hi-Z 4096) |
| `vegViewRadius` (14), `vegHighDetailRadius` (4) | VegetationSystem | live-sûr |
| `grassViewRadius` (3) | GrassSystem | live-sûr |
| `grassBladeSpacing` (0.15) | GrassSystem.cpp | rescatter via `regenerate()` (pop-in bref — knob dev) |
| `grassFadeStart/End` (140/190), `grassFarDensity` (0.20), `grassThinEnd` (70) | dupliqués CPU+shader | live via **APPEND FrameUbo `grassLodInfo`** (les DEUX côtés lisent la même source) |
| `shadowResolution` (2048 ; 1024/2048/4096) | ShadowMapper | recreate (destroy+create ; le NOMBRE de cascades reste compile-time — Mat4[3] structurel) |
| `volumetricSteps` (20), `godRayTaps` (48), `contactSteps` (12), `ssaoSamples` (à vérifier dans ssao.frag) | consts shaders | live via **APPEND FrameUbo `postStepInfo`** (boucles bornées par uniform) |
| `reflectionScale` (0.5 ; 0.25-0.5) | ensureOffscreenTarget | recreate |
| `renderScale` (1.0 ; 0.5-1.0) | — | ci-dessous |

**Render scale :** offscreen HDR à `width*scale` — le tonemap
échantillonne déjà la texture en 0..1 linéaire vers le backbuffer →
upscale gratuit, zéro changement de shader. **Deux pièges
obligatoires :** `gpuOcclusion.resize` et `composeFrameUniforms` doivent
recevoir les dimensions OFFSCREEN (`uScreenInfo` pilote les texels
SSAO/eau/god rays) ; l'UI reste en full-res backbuffer. Vérifier au
debug-buffer viewer que SSAO/volumétrique restent alignés à 0.5.

Sliders/combos dans une section « Qualité / Perf » de `drawRenderPanel`.

## Briques P2+ — candidates (ordre PROVISOIRE, réordonné par P0)

### P2 — PostFx : early-outs à intensité nulle (S, byte-identique)
Sauter god rays (`godRayIntensity == 0` OU soleil hors écran),
volumétrique, SSAO, bloom à intensité 0 — et donc en intérieur. Piège :
le tonemap sample ces textures inconditionnellement → clear-once neutre
au premier skip (noir pour les additifs, blanc pour SSAO — motif
`clearContactShadows` existant, PostFx.cpp:267-278).

### P3 — Réflexion planaire (S×3, tout togglé)

> **FAITE (2026-07-10) — validation dev en attente.** P5c mesuré par le
> dev : `shadows` 5,51 → **3,6 ms** (-1,9 ms, conforme). P3 livre :
> **auto-skip** (checkbox à côté de Water reflections, ON par défaut —
> le miroir ne se rend que si un chunk résident sous seaLevel est dans
> le frustum ; cas limite mer-à-l'horizon = LE point à vérifier sur la
> crête) et **Reflection scale** (slider 0,25-0,5, recreate du target).
> À relever : `reflection` en vue mer (inchangé ~1,7 ms), en vue
> montagne (→ ~0 si le skip mord), et à 0,25 (~0,6 ms attendu).
1. **Skip si pas d'eau visible** : la mer est le seul consommateur de
   `uReflection` (les volumes placés utilisent le fresnel ciel). Test :
   aucun chunk résident avec `lo.y < seaLevel` dans le frustum →
   réflexion off (le shader a déjà le fallback ciel). **Cas limite
   honnête :** mer à l'horizon AU-DELÀ du ring résident (960 m) — le
   test la manque → toggle « auto-skip » + validation dev sur la crête.
2. **Quarter-res** (`reflectionScale` 0.25) : miroir plus flou, slider.
3. **Demi-cadence** (1 frame sur 2, texture persistante) : risque de
   « swim » caméra rapide — seulement si encore cher après 1+2.

### P4 — Micro-passes de la passe principale (S/M)
- **Pluie** : `draw(ceil(3000 × rainIntensity) × 6)` — le hash par streak
  rend le préfixe uniforme. Look : moins de streaks à faible pluie au
  lieu de streaks plus pâles (probablement mieux — validation dev).
- **Cumulonimbus** : FBM 4 octaves/pixel sur 8 billboards — coût réel
  inconnu, MESURER d'abord. Si cher : bruit baké 256² (motif cloud
  bake), scroll dans le shader. A/B.
- **Instancing des PNJ skinnés : DÉFÉRÉ** (quelques PNJ — n'apparaîtra
  pas en tête de table).

### P5 — Cache CSM (le gros morceau — SEULEMENT si `shadows` domine)

> **BASELINE DEV (2026-07-10, RTX 4070 SUPER, spot extérieur) : total
> GPU 10,0 ms, `shadows` = 5,51 ms (55 % !), reflection 1,70, mainVeg
> 1,81, mainGrass 0,36, postfx cumulé ~0,3 (→ P2 ABANDONNÉE par la
> règle < 0,5 ms).** L'ordre devient : P5(c) → P3 → knobs veg P1.
>
> **P5(c) round-robin FAIT (même jour, toggle « CSM round-robin » du
> panneau Rendering, ON par défaut — validation visuelle dev en
> mouvement attendue + relever le nouveau `shadows`)** : cascade 0
> chaque frame, cascades 1/2 en alternance ; une cascade sautée garde
> SA matrice précédente (receiver + caster UBO — la depth stale doit
> être échantillonnée avec la matrice qui l'a dessinée) ; un pas de
> soleil re-rend tout. Attendu ~-2 ms sur `shadows`.
**Piège dit honnêtement :** les cascades re-fittent sur le frustum
caméra chaque frame — en déplacement les matrices changent presque
chaque frame et invalident tout cache naïf (le snap texel n'absorbe que
la translation).
- (c) **Round-robin** (S) : cascade 0 chaque frame, 1 et 2 en
  alternance — ~40 % de casters en moins, ~10 lignes. Ombres lointaines
  à demi-cadence → A/B obligatoire + session dev en mouvement.
- (b) **Split statique/dynamique** (L, risque moyen-haut) : 2ᵉ array
  3×2048² statique (terrain+veg) re-rendu seulement sur pas de soleil /
  résidence / re-fit ; copie statique→actif chaque frame
  (glCopyImageSubData, le RHI l'a) + casters dynamiques par-dessus.
  Nécessite des matrices GELÉES par hystérésis de re-fit (fitter avec
  marge ~1,15×, re-fitter quand la tranche vraie sort de la sphère
  élargie). +16 Mo VRAM. Seulement si ≥ 2-3 ms restants après (c).
- (a) **Skip total en contemplation** : gratuit par-dessus (b).

### P6 — Stall upload terrain 14 ms (S/M)
Re-mesurer d'abord (log one-shot octets/upload dans pumpUploads). Si
cumul → budget en OCTETS dans `ChunkStreamer::pump` (le « au moins un
upload » actuel laisse passer les gros LOD0 ~200 Ko). Si UN createBuffer
stalle dans le driver → staging ring persistant-mappé
(GL_MAP_PERSISTENT_BIT + glCopyBufferSubData), backend-only. Look :
neutre.

### P7 — Herbe : knobs + mesure SEULEMENT (structurel DÉFÉRÉ)
La refonte VISUELLE de l'herbe est prévue (33a attend dessus) : tout
travail structurel (ruban→quad lointain, courbe de décimation, shader
distance) serait jeté. Ce chantier livre les knobs P1 et le sous-scope
`mainGrass` — le budget d'entrée de la refonte (« l'herbe coûte X ms »).

## Ordre proposé (à réordonner par la table P0)

| Ordre | Brique | Effort | Gain attendu (HYPOTHÈSE) |
|---|---|---|---|
| 1 | P0 timers GPU | M | l'outil — prérequis absolu |
| 2 | P1 knobs + render scale | M | budget trouvable en live ; levier brut |
| 3 | P2 postfx early-outs | S | intérieurs + sliders francs |
| 4 | P3 réflexion | S | gros ratio gain/effort extérieur potentiel |
| 5 | P4 pluie/cumulonimbus | S/M | storm frames |
| 6 | P5 cache CSM (c puis b) | S puis L | gros si `shadows` domine |
| 7 | P6 stall upload | S/M | tue le spike 14 ms |
| 8 | P7 herbe | — | mesure only, alimente la refonte |

## Protocole de vérification (session dev)

Build Release, 1080p, HUD perf ouvert, toggles par défaut. **La ligne
« total GPU » de la baseline DÉFINIT la barre** ; cible proposée :
**≤ 16,6 ms GPU (60 fps) sur les 4 spots à renderScale 1.0** — à
confirmer/amender par le dev à la lecture de la baseline (GPU inconnu).

**Les 4 spots (relevés une fois, re-relevés à chaque brique) :**
1. **Crête extérieure** vue mer + horizon (réflexion + rayon plein +
   herbe + veg) — pire cas extérieur ;
2. **Village** (meshes + PNJ + casters) ;
3. **Hall intérieur** (`kDevStartInterior`) ;
4. **Storm extérieur** (pluie 3000 + cumulonimbus + occlusion pluie).

Par spot : moy + max par passe, total, fps min sur 30 s avec un 360°,
puis 1 min de déplacement rapide (le spike upload). Acceptation par
brique : (a) la passe CIBLE baisse d'un montant mesuré et annoncé ;
(b) toggles off → extérieur inchangé (captures A/B) ; (c) aucun nouveau
spike CPU/GPU au log ; (d) suite headless verte + smoke-run.

## Inconnues assumées

Aucun chiffre GPU n'existe ; GPU du dev inconnu (la table rend le budget
explicite) ; coûts réels cumulonimbus/réflexion/herbe mesurés à P0 avant
toute décision ; fiabilité des timer queries selon driver (si aberrant,
le HUD le montrera — max >> moy).

---

## Chantier — Parallélisme GPU (2026-07-24)

> Question dev post-CLUSTERED : « que peut-on paralléliser pour le
> rendering (mainPass peut-être) ? ». Diagnostic : la frame est
> GPU-bound (sim ≤ 2 ms ; la mesure ci-dessous montre le CPU bloqué en
> back-pressure) — paralléliser le CPU ne raccourcit rien. Le gisement
> est le RECOUVREMENT des passes SUR le GPU : la baseline prouve que la
> frame est aujourd'hui STRICTEMENT SÉRIELLE (somme des passes ≈ total).
> Le mainPass lui-même n'est pas un problème de parallélisme (le raster
> est déjà parallèle) : son coût est du volume de travail — les réponses
> sont les briques P1-P7 ci-dessus (knobs, imposteurs, refonte herbe).

### PG0 — Baseline Release M1 — ✅ MESURÉE (2026-07-24, partielle)

Outil ajouté : une ligne `gpu budget (avg/max ms, 120f)` loggée UNE fois
à la frame 2000 (warmup passé, fenêtre pleine) — la table F6 sans les
yeux sur le HUD, pour les sessions scriptées. Conditions : **Release,
M1/MoltenVK, spot de spawn, caméra par défaut, ~100 s immobile** — le
protocole 4-spots complet reste au dev.

| Passe | OFF clustered (ms avg/max) | ON clustered (défaut) |
|---|---|---|
| **frame totale** | **34,7 / 44,2** | **39,6 / 67,2** |
| shadows (CSM) | 7,7 / 15,4 | 8,6 / 20,5 |
| mainPass | 9,3 / 10,7 | 10,3 / 17,0 |
| rcInject+rcBuild+rcMerge | 9,9 cumulés | 11,6 cumulés |
| reflection | 4,3 / 11,0 | 5,0 / 12,6 |
| clusterCull | — | 0,35 / 0,95 |
| copyHizWater | 1,1 | 1,2 |
| postfx cumulé (bloom/volum./contact/…) | ~1,7 | ~1,8 |

Lectures honnêtes : (1) **la somme des passes ≈ le total** → zéro
recouvrement, tout est sériel ; (2) l'écart ON/OFF est UNIFORME, y
compris sur des passes que le clustered ne touche pas (shadows, RC,
reflection) → variance thermique M1 entre runs successifs, pas un
surcoût clustered (son coût propre mesurable = clusterCull 0,35 ms) ;
(3) les « frame spike CPU postfx 25-35 ms » qui inondent le log sont la
**back-pressure GPU** (le CPU attend dans l'enregistrement postfx), pas
un coût CPU réel ; (4) les max de shadows (15-20 ms) sont les pas de
soleil quantisé qui re-rendent les 3 cascades d'un coup ; (5) à
~35-40 ms GPU au spot de spawn, le M1 est LOIN de la barre 16,6 ms —
les briques P1-P7 restent pertinentes en complément du recouvrement.

### Le graphe de dépendances réel (début de frame)

| Passe | Dépend de | Type |
|---|---|---|
| clusterCull | LightsUbo seul | compute |
| cloudBake 512² | rien | raster |
| key shadows (atlas ×4) | rien | raster |
| CSM ×3 | rien | raster |
| RC inject→build→merge | CSM + cloud map | compute (chaîne interne sérielle) |
| réflexion planaire | CSM + cloud map | raster |
| mainPass | tout ce qui précède | raster |
| froxels (dans postfx) | CSM + key shadows + cascade 0 + listes clusters | compute |

clusterCull, cloudBake, key shadows et CSM sont MUTUELLEMENT
indépendants ; la chaîne RC (~10-12 ms) et la réflexion (~4-5 ms) ne
dépendent que de CSM+nuages et sont indépendantes ENTRE ELLES. Le
recouvrement théorique parfait masquerait ~10 ms ; le réaliste
(compute RC ↔ raster réflexion/début de mainPass) en masque une partie
— à mesurer, c'est tout l'objet de PG1.

### Le verrou : `memoryBarrier()` global

`VulkanDevice.cpp` (`VulkanCommandBuffer::memoryBarrier`) émet
`COMPUTE → ALL_COMMANDS` — délibérément large, c'est le contrat RHI
actuel. Conséquence : chaque frontière INTERNE de la chaîne RC (5
barrières), des froxels (2) et du clusterCull fence tout le raster qui
suit. Le GPU n'a jamais la PERMISSION de chevaucher. Sur Apple Silicon
le chevauchement compute↔raster entre encoders indépendants est
précisément ce que le matériel fait bien — mais MoltenVK traduit en
hazard-tracking Metal : le recouvrement effectif n'est pas garanti,
d'où la règle « mesurer » (le F6 le montre directement : total < somme
des passes = ça chevauche).

### Effectivité sur GPU dédié (question dev, 2026-07-26)

Ces briques ne sont PAS des optimisations « spécifiques M1 » : c'est le
M1 qui est le cas dégradé (une famille de queues, MoltenVK) du modèle
que le PC exploitera à plein. PG1 : sur NVIDIA/AMD le matériel exécute
compute et raster en concurrence sur la même queue — seules les
barrières larges l'en empêchent ; les barrières fines sont le chemin
canonique du Vulkan PC (sur le backend GL PC actuel en revanche :
quasi-neutre, le driver ordonnance lui-même). PG2 : gain absolu plus
modeste sur GPU rapide, mais prérequis structurel de PG3. PG3 (queues
compute + transfert dédiées) : levier quasi exclusivement PC — l'async
compute de la GI/volumétrique et le DMA des uploads (le stall P6) sont
le pattern standard des moteurs modernes. Réserves : la baseline RTX de
ce doc (10 ms, 2026-07-10) est antérieure à RC/froxels/clustered et
mesurée sur GL — re-baseline obligatoire au retour PC ; et sur RTX
`shadows` dominait (raster↔raster) — là-bas le complément reste P5b et
le GPU-driven (cull Hi-Z en indirect draws sans readback).

### Briques

- ✅ **PG1 — Barrières à portée fine — FAITE (2026-07-26)** :
  `BarrierDst` (Compute/Fragment/Vertex/Transfer/All, OR-ables) dans le
  RHI, `memoryBarrier(dst)` sur les deux backends (Vulkan : dstStages
  scopés ; GL : bits de visibilité par mode de lecture, IMAGE_ACCESS
  ajouté), resserrage des 9 frontières : RC interne → Compute (le merge
  final → Compute|Fragment), froxels inject→integrate → Compute,
  integrate→apply → Fragment, clusterCull → Fragment|Compute, Hi-Z →
  Compute puis Transfer (le verdict ne nourrit que la copie staging).
  Le probe GI one-shot garde All (readback CPU).
  **Mesuré (Release M1, spot de spawn, 2 runs dos à dos)** : frame
  **39,6 → 26,4-26,8 ms** (-33 % vs clustered ON pré-PG1 ; -24 % vs le
  meilleur run pré-PG1) — RC 11,6→7,2, mainPass 10,3→6,8, shadows
  8,6→6,2, reflection 5,0→2,8 : les barrières globales drainaient le
  pipeline à chaque frontière. Nuance de lecture : sous recouvrement,
  les timestamps par passe deviennent flous (les passes se fondent) —
  le chiffre de confiance est le TOTAL de frame. Validation :
  **synchronization validation layer active (« Current Enables »
  vérifié) et PROPRE (0 SYNC-HAZARD, 30 s)** ; 522 tests verts ; GL
  compile (backend non exécutable sur macOS — à smoker au retour PC).
  **Confirmation dev en jeu (2026-07-26) : forêt 15-17 → 22 fps**
  (~60 → ~45 ms — le pire cas végétation profite au même ratio que le
  spot de spawn). Note de protocole : le spawn regarde un mur — cadrage
  CONSERVÉ tel quel pour que les lignes « gpu budget » restent
  comparables entre elles (revert du volte-face, 2026-07-26).
- ✅ **PG2 — RC pipelinée N−1 — FAITE (2026-07-26)** : la chaîne RC
  s'enregistre en FIN de frame (`recordGiUpdate`, extrait de render()) ;
  la frame consomme la cascade 0 de la frame précédente (la RC est déjà
  temporelle — latence d'une frame invisible). Mécanique de barrières :
  `readBarrier(src)` ajouté au RHI (fence WAR d'exécution pure — les
  lectures de l'ancienne cascade finissent avant la réécriture ; no-op
  GL, l'ordre y est implicite) ; la chaîne se termine SANS clôture ; la
  frame suivante pose la **clôture consommateurs**
  (`memoryBarrier(Fragment|Compute)`) à l'ancien slot post-CSM, avant le
  premier lecteur GI — tout ce qui est entre les deux (composite,
  present, clusterCull, cloudBake, CSM, key shadows N+1) a la
  permission de recouvrir la chaîne. Garde-fous : probe GI et debug
  view posent leur propre barrière quand pipeliné. Toggle « Pipelined
  GI (frame N-1) » (RcTuningForm.pipelined, défaut ON, Save).
  **Mesuré (Release M1, spawn, 2 runs)** : total **26,4 ms — NEUTRE**
  vs PG1 (le composite recouvre bien : 0,33 → 0,02 ms, mais MoltenVK ne
  laisse pas le recouvrement traverser la frontière de soumission —
  la chaîne en fin de frame N ne chevauche pas le front de N+1).
  Résultat assumé : le gain de PG2 est STRUCTUREL — c'est le prérequis
  de PG3 (async compute PC), et tout driver qui recouvre à travers les
  submits (NVIDIA/AMD) l'encaissera. Validation : sync validation layer
  active et propre (0 SYNC-HAZARD), 522 tests verts. Nota : en mode
  legacy (pipelined OFF), la WAR théorique inter-frames (lectures N−1
  vs réécriture N) reste comme avant PG2 — surveillée par la sync
  validation, propre à ce jour.
- ✅ **PG3 — Async compute GI — FAITE (2026-07-26)**, et la prémisse
  « le M1 n'a qu'une famille » était FAUSSE : MoltenVK expose plusieurs
  familles (chacune sa MTLCommandQueue, que l'Apple Silicon ordonnance
  en concurrence ; `MVK_CONFIG_SPECIALIZED_QUEUE_FAMILIES` marque même
  des familles compute/transfert dédiées). Livré : sélection d'une
  famille compute ≠ graphics (caps `asyncCompute`), sémaphore TIMELINE
  (feature 1.2 activée), soumission croisée — graphics N signale, la
  chaîne RC (sur la 2ᵉ queue) attend et signale, graphics N+1 attend
  cette valeur À SES STAGES CONSOMMATEURS seulement (fragment, compute,
  depth-writes — le vertex et les transferts recouvrent) ; partage
  CONCURRENT des ressources (simplicité prototype, Metal l'ignore) ;
  fenêtre de routage explicite des updateBuffer vers le CB compute
  (asyncComputeCmd/endAsyncCompute — la sync validation a attrapé les
  vertex UI qui fuyaient dedans, et la libération des stagings sur foi
  de la seule fence graphics : le wait timeline du slot vit dans
  beginFrame). Toggle « Async compute GI (2nd queue) »
  (RcTuningForm.asyncCompute, défaut ON, exige pipelined).
  **Mesuré (Release M1, spawn, 2 runs) : frame 26,4 → 19,2 ms** — la
  chaîne RC (~7,5 ms) est ENTIÈREMENT masquée ; les spikes CPU suivent.
  **Total chantier : 34,7 → 19,2 ms (-45 %).** Limites notées : les
  scopes GpuProbe rc* sont coupés en async (timestamps de l'autre
  queue non instrumentés — le coût RC se lit par l'A/B du toggle ;
  follow-up : timestamps par queue) ; la vue debug RC peut montrer un
  champ en cours d'écriture (dev only) ; la queue TRANSFERT dédiée
  (uploads streaming) reste un lot PC/post-démo. Validation : sync
  layer active, 0 SYNC-HAZARD, 0 erreur, 522 tests verts.
- **PG4 — Render thread : DIFFÉRÉ explicitement.** Le seam Phase 5
  (snapshot strict) le garde à ~une brique de distance ; il paie quand
  la sim CPU grossira (chantier « vivant »), pas à ≤ 2 ms. Idem
  l'enregistrement parallèle des command buffers (revue V8+ point 5 :
  prématuré) — à ressortir si `render()` CPU monte au FrameProbe.

Règles du chantier : celles du présent doc (mesurer d'abord,
look-neutre ou togglé, tout par le RHI, biais prototype) + la
synchronization validation layer obligatoire sur toute brique barrière.
