# TERRAIN-TEXTURING — journal du chantier

> Brief d'origine : `docs/AUDIT/TERRAIN-TEXTURING.md` (architecture par bandes
> de fréquence, matériaux tuilés 512², height blending, teinte macro, POM).
> Ce journal consigne les décisions, l'état des briques et les verdicts
> lockstep. Démarré le 2026-08-02.

## Décisions actées (2026-08-02, discussion dev)

1. **Splat hybride C.** Les poids de mélange restent calculés in-shader
   (procédural, modèle Andersson/Frostbite), enrichis par des canaux bakés
   montés au GPU (biome, lithologie `hardness`, wetness). Le calcul des poids
   est isolé (fonction GLSL + cœur CPU unique) pour qu'une couche d'override
   peinte (B10, mode Scénario) puisse s'y superposer plus tard sans refonte.
   PAS de splatmap stockée : le monde sandbox est généré/streamé, la bande
   « unique par région » = les masques u8 à 16 m/texel de `TerrainRegion`.
2. **Pipeline offline d'abord** (chantier A), phases visuelles ensuite
   (chantier B). Les briques B1-B4 restent indépendantes du pipeline.
3. **Matériaux : prototype comparatif** — tuiles procédurales enrichies VS
   CC0 512² retravaillées, jugé à l'écran avec le height blending actif.
4. **Conteneur maison `.mtex`**, pas KTX2 (dérogation à la table CLAUDE.md §3,
   actée) : header POD + payload au byte-layout exact de
   `createTexture(pixelsIncludeMips)` — 1 fread, zéro transcodage, zéro dep.
   KTX2 redevient une option si un besoin d'interop apparaît.
5. **Vulkan-only.** Le dev envisage d'arrêter le support GL (« Vulkan
   fonctionne partout »). Les formats BC et l'upload de mips ne sont PAS
   implémentés dans les backends GL : leur cap `textureCompressionBC` reste
   false, le terrain y garde le splat procédural.

## Chantier A — pipeline offline (FAIT 2026-08-02)

- **A1 RHI** : `TextureFormat` += BC7_SRGB / BC7_UNORM / BC5_UNORM /
  R16_UNORM ; helpers d'arithmétique de blocs (`formatBlockBytes`,
  `mipLevelBytes`, `textureDataBytes` — la SEULE autorité de taille,
  partagée backends/loader/cooker/tests) ; cap `DeviceCaps::
  textureCompressionBC` ; `TextureDesc::pixelsIncludeMips` (payload
  mip-major, layers contigus par mip → 1 `VkBufferImageCopy` par mip).
  R16_UNORM = memcpy u16 brut (≠ contrat R16F f32→f16). `generateMipmaps`
  refuse d'écraser une chaîne offline. BC vérifié natif sur M1/MoltenVK
  (famille apple7 macOS).
- **A2 cooker** : dep CPM `bc7enc_rdo` (bc7enc.cpp + rgbcx.cpp seulement,
  linkée au cooker) ; `engine/assets/CookedTexture.{hpp,cpp}` (headless,
  codes de format on-disk STABLES découplés de l'enum rhi) ; sous-commande
  `cooker cook-terrain-materials <manifest> <outDir>` : décode PNG
  (`stbi_load_16` pour la height), downsample 512² (stb_image_resize2,
  Mitchell, EDGE_WRAP — les tuiles sont périodiques), packing ORM (ou maps
  séparées ao/roughness/metallic), flip Y optionnel des normales, chaîne de
  mips rééchantillonnée depuis la base (pas de halving successif), BC7
  perceptuel (albedo) / BC7 linéaire (ORM) / BC5 (normal RG) / R16 brut.
  Piège trouvé : `bc7enc_compress_block_params_init_linear_weights` ne
  règle QUE les poids — l'init complète d'abord, sinon segfault.
- **A3 runtime** : `LandscapeTuningForm` += 4 guids réfléchis
  (`terrainAlbedoArray`/`Normal`/`Orm`/`Height`), déclarés dans `[assets]`
  de `landscape.toml` → `textures/terrain/terrain_*.mtex`. La scène résout
  guid→chemin (le renderer ne voit ni Form ni VFS) et remplit
  `RendererConfig::terrain*Path` ; `TerrainSystem::create` charge les 4
  arrays si `caps().textureCompressionBC` (albedo bindé à la place du splat
  procédural ; normal/ORM/height résidents, en attente des briques B).
  Fallback procédural sur toute absence/échec. Gate : `arrayLayers` doit
  == `SplatLayer_Count`.
- **Corpus** : 5 matériaux CC0 ambientCG 1K (ordre = `SplatLayer`) :
  Grass001, Rock058, Snow010A, Ground054 (sand), Rock051 (cliff). Sources
  dans `assets-src/terrain/` (gitignoré) ; manifest
  `game/data/base/terrain/materials.toml` ; sortie cookée commitée sous
  `game/data/base/textures/terrain/` (~8,5 Mo pour les 4 arrays × 5
  couches, mips comprises). ambientCG livre `NormalGL` (+Y OpenGL) → pas
  de flip.
- **A4 tests** : `tests/CookedTextureTest.cpp` — helpers de blocs (arrondi
  4×4 aux petites mips, multiplicateur layers), round-trip save→load,
  rejets (payload incohérent, magic invalide, fichier tronqué).

## Chantier B — phases visuelles

- **B1 poids isolés (FAIT 2026-08-03)** : la règle des poids vit UNE fois de
  chaque côté — `shaders/terrain_weights.glsl` (fonction pure de scalaires,
  le fetch wander reste dans terrain.frag) et `materialWeightsCore` dans
  `TerrainNoise.cpp` (struct `WeightRuleInputs` ; les divergences des trois
  fonctions publiques — biome dans `At`, wander dans `Shaded` — deviennent
  des paramètres, signatures publiques inchangées). Vérifié bit-identique :
  601 tests, 635 998 assertions inchangées.
- **B2 height blending (FAIT 2026-08-03)** : `buildSplatHeightPixels()`
  (heights procédurales R16F corrélées aux albedos par les mêmes seeds),
  array height bindé en **3** du groupe 1 (cooked R16_UNORM ou procédural),
  `shaders/terrain_blend.glsl` (`blendHeights` 5 couches, skip des couches
  à poids ~0 → 2-3 fetchées), `uSplatDetailInfo` appendé au FrameUbo
  (x = profondeur de bande ; yzw réservés detail fade/POM), knob
  `splatBlendDepth` (LandscapeTuningForm, 0.15, 0 = blend plat). Footsteps
  ne suivent PAS (verdict documenté dans TerrainNoise.hpp).
- **Pièges MoltenVK trouvés en B2** : (1) les bindings de samplers sont un
  espace GLOBAL par closure d'includes — le binding 2 de terrain.frag
  appartient à `uCloudMap` (clouds.glsl) ; carto avant d'en choisir un
  (pris : 0,1,2,6,7,11 ; le shade-map B3 prendra 4-5). (2) Un échec de
  compilation MSL **empoisonne `vulkan-pipeline-cache.bin`** — après le
  fix, supprimer le cache sinon `vkCreatePipelineCache` échoue en boucle.

- **B3 TerrainShadeMap (FAIT 2026-08-03)** : nouveau
  `TerrainShadeMap.{hpp,cpp}` (pattern TerrainLightMap) — bake worker 512²
  / span 3072 m centré caméra, republié au stray ou au changement de
  `contentStamp`. T0 = tint.rgb (blanc neutre jusqu'à B4) + wetness ;
  T1 = rockiness / snowLineOffset ((v·255−128)×8 m) / sandiness / beach.
  **Jamais l'index biome** : la résolution id→attributs se fait au compose
  (`terrain::regionShadingAt` — LA source unique, consommée par le bake ET
  les miroirs CPU). Samplers 4-5, groupe 7 (passes main + réflexion),
  `uTerrainShadeMapInfo` appendé ; hors span → entrées neutres.
  Parité règles biome livrée avec : `terrain_weights.glsl` étendu
  (rockShift/sandiness/beach/snowOffset — ex-différés « cliffiness par
  biome » côté rockiness), et `materialWeightsShaded` suit (footsteps =
  biome, verdict voulu) ; `materialWeightsAt` inchangé (zéro reseed).
  `hardness` (lithologie) : publication TRG différée — la teinte v1 se
  compose sans elle.

- **B4 teinte macro + raccords (FAIT 2026-08-03)** : composition dans
  `regionShadingAt` — climat biome (temperature/wetness) → couleur, dérive
  d'aridité ~700 m (fbm seedée) qui casse la tapisserie sans tracer les
  frontières de biome, froid → pâle bleuté, humidité (masque wetness) →
  assombrissement « terre mouillée » (le différé v2 passe par la teinte,
  donc herbe et GI le partagent). Force : `terrainTintStrength`
  (LandscapeTuningForm, 0.3, garde-fou ≤ ~0.4) → `uSplatDetailInfo.y`,
  la map stocke la couleur pleine force. Raccords : **herbe** (le bake
  d'albédo racine multiplie par le même mix(1,tint,strength) ;
  `GrassScatterTuning::tintStrength` synchronisé + regen, comme
  splatUvScale), **GI** (la tuile albedo RC multiplie par le même tint ;
  re-bake sur changement de strength). **FarTerrain** : non touché — à
  vérifier visuellement (le brouillard couvre sans doute le raccord) ;
  si mismatch, tint CPU au build du far mesh. Tap live water-info : différé
  (la wetness bakée suffit pour la berge).

## Retour de validation dev (2026-08-03) et correctifs

- **Textures : OK** (verdict dev). Sable : présent sur les plages de mer ✓ ;
  absent aux lacs/rivières d'altitude = design actuel (aucune règle de sable
  aux berges — brique future possible via le canal wetness/beach du
  ShadeMap si voulu).
- **FIX wander** : le terme de wander des lignes sable/neige échantillonnait
  le canal vert de la couche herbe (calibré −0.67 pour la tuile procédurale,
  moyenne 0.36 linéaire). Avec l'albedo CC0 (0.106 linéaire) : biais faux de
  −0.25 ET bruit par pixel sur les frontières (la « mauvaise transition
  neige/herbe » vue par le dev) ET miroir CPU désynchronisé. Remplacé par
  `borderWander` — bruit de lattice analytique partagé bit-à-bit GLSL/C++
  (hash murmur = core::hashU32), biais historique −0.31 ± 0.1, indépendant
  du set de matériaux. `splatWander` supprimé.
- **FIX perf chargement** (rallongement 70-100 % constaté par le dev) : la
  composition de teinte (fbm) était payée par TOUS les appels
  `materialWeightsAt/Shaded` du scatter au warmup. Scindé :
  `regionFieldsAt` (champs, chemin chaud) / `regionShadingAt` (champs +
  tint fbm — bakes et lattices épars seulement).
- **Recook auto** (demande dev) : target CMake `terrain-materials` — si
  `assets-src/terrain` est présent et que le manifest ou une source PNG est
  plus récent que les `.mtex`, le cook relance avant `game-data`. Absent
  (clone frais) : les `.mtex` commités servent tels quels.

À VENIR : B5 prototype comparatif (toggle A/B panel : reconstruire les
arrays procéduraux enrichis vs cooked à chaud) → B6 detail normal
(TBN analytique des UV planaires) → B7 POM (garde-fous M1 :
uSplatDetailInfo.zw réservés) → B8 stochastique (option). Détail : brief
§5. À re-valider par le dev : transition neige/herbe (le fix wander devrait
la réparer) et temps de chargement.

Note DA transitoire : depuis le chantier A, l'albedo terrain par défaut est
la bibliothèque CC0 cookée (landscape.toml pointe les .mtex) — le raccord
couleur herbe/sol (grassAlbedo procédural) ne matche plus sur ce chemin
tant que B5 (toggle + jugement) et B4 (tint partagé) ne sont pas passés.
Pour retrouver l'ancien look : vider les 4 guids terrain*Array.

### Verdicts lockstep déjà tranchés (à respecter en implémentant)

- **Footsteps ≠ height blending** (B2) : `materialWeightsShaded` ne suit pas
  la redistribution décimétrique des poids dans les bandes de transition.
- **Footsteps = biome** (B4) : quand les règles biome montent au GPU, le
  miroir CPU les gagne aussi — le pas sonne comme le sol visible.
- **Scatter intouchable** : `materialWeightsAt` garde ses masques historiques
  (les reseeder déplacerait chaque buisson).
- **GI = teinte macro, GI ≠ detail normals** (B4/B6) : les Radiance Cascades
  voient le tint (un canyon ocre bounce ocre) mais gardent la normale
  analytique.
