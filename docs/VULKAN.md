# Chantier VULKAN — backend Vulkan + MoltenVK (renderer final)

> Journal du chantier. Plan complet approuvé le 2026-07-18. Vulkan est le
> renderer **final** (pas seulement un shim macOS) : GL4.6 en fallback PC,
> GL4.1 low-spec un jour, macOS = Vulkan seul. Sélection **runtime** (§2.1,
> chaîne de préférence Vulkan→GL) + gating **compile-time** de quels backends
> sont compilés (`MEADOWS_RHI_VULKAN`). Voir aussi la mémoire projet
> `renderer-backend-lineup`.

## Pourquoi

Dev passé sur MacBook Air M1 : Apple GL figé à 4.1 (pas de compute/SSBO/HDR/
volumes) → le renderer 3D (GI Radiance Cascades, culling GPU, postfx) ne peut
tourner que via **Vulkan + MoltenVK**. Le RHI (`engine/rhi/`) était déjà conçu
Vulkan-ready (interface explicite, chaque commentaire donne l'équivalent VK).

## Environnement (vérifié 2026-07-18)

- SDK LunarG installé : `~/VulkanSDK/1.4.335/`, loader `/usr/local/lib/
  libvulkan.dylib` (1.4.335), `libMoltenVK.dylib`, headers `/usr/local/include/
  vulkan`, `glslang` + SPIRV-Tools présents. `VULKAN_SDK` non exporté mais tout
  est sous `/usr/local` → `find_package(Vulkan)` le trouve.
- Socle GL41/2D compile et tourne déjà sur ce Mac (824 objets, binaires
  présents).

## Décisions d'implémentation

- **volk reporté** : V0-V1 utilisent le loader lié (`Vulkan::Vulkan` via
  `find_package`), pas volk. volk (chargement dynamique, moins d'overhead de
  dispatch) = optimisation ultérieure, pas un besoin de bring-up.
- **Ordre fenêtre/device** : le flag SDL (`SDL_WINDOW_OPENGL`/`_VULKAN`) est
  figé à la création → le backend est résolu **avant** `Window::create`.
  `WindowDesc.api` (enum `platform::GraphicsApi`, platform-clean) porte le
  choix. La chaîne de fallback Vulkan→GL (avec recréation de fenêtre si l'API
  diffère) arrive en **V1**, quand Vulkan peut présenter.
- **GL non gaté pour l'instant** : les backends GL restent toujours compilés
  (le 2D/dev-UI/démo tournent dessus) ; seul Vulkan est derrière
  `MEADOWS_RHI_VULKAN`. Le gating macOS = Vulkan-seul viendra quand Vulkan sera
  fonctionnel (sinon on casserait le build GL41 qui tourne).

## Avancement

### V0 — Build, deps & câblage de sélection — ✅ FAIT (2026-07-18, `76004a7`)
- `Rhi.hpp` : `Backend::Vulkan` ajouté.
- `platform` : `WindowDesc.api` (`GraphicsApi::OpenGL|Vulkan`) → flag SDL suit
  le backend (`Window.cpp`).
- CMake : option `MEADOWS_RHI_VULKAN` (défaut ON sur APPLE), `find_package(
  Vulkan)`, source `backends/vulkan/VulkanDevice.cpp` + lien `Vulkan::Vulkan`
  + define `MEADOWS_RHI_VULKAN` sur `meadows-render`.
- `Device.cpp` : branche factory `case Backend::Vulkan` (gatée).
- `Engine.cpp` : API fenêtre dérivée de `config.backend` avant `Window::create`.
- **Backend Vulkan** : `VulkanDevice` (pimpl, toutes les virtuelles stubées) +
  `createVulkanDevice`. V0 : `create()` prouve que le loader linke
  (`vkEnumerateInstanceVersion`, log la version) puis **décline → fallback GL**.
  Défaut backend inchangé (GL) → le Mac continue de tourner en GL41.
- **Validé sur M1** : Vulkan 1.4.335 trouvé, `VulkanDevice.cpp` compilé dans
  `libmeadows-render.a` (symbole `rhi::createVulkanDevice` exporté), build
  complet vert, suite headless **516/516**.

**Deux leçons de bring-up (coûteuses à redécouvrir) :**

1. **Prérequis imprévu — le M1 ne compilait rien.** `develop` n'avait jamais été
   bâti sous **Apple clang 21** (code écrit sous gcc/clang plus ancien). Deux
   constructions non conformes bloquaient TOUT le build (même le GL41/2D) :
   `SkySystem::evaluate(const Weather& = {})` (les initialiseurs par défaut
   d'une classe imbriquée ne sont pas disponibles dans la classe englobante —
   règle standard qu'Apple clang applique) et `std::chrono::clock_cast`
   (non implémenté dans la libc++ d'Apple). Corrigés à part (`db2eb94`).
   → *Toute validation M1 suppose ce build vert ; re-vérifier après un merge
   venant d'une session Linux.*
2. **RPATH du loader Vulkan.** L'install name est `@rpath/libvulkan.1.dylib` :
   sans `LC_RPATH`, les exécutables refusent de démarrer (« no LC_RPATH's
   found ») — y compris `meadows-tests`. `CMAKE_BUILD_RPATH` reçoit le dossier
   de `${Vulkan_LIBRARY}`, posé AVANT la définition des targets.

### V1 — Instance / device / swapchain / surface (frame « clear ») — ✅ FAIT (2026-07-18)

**Vulkan tourne sur le M1 via MoltenVK** : `Vulkan device ready: Apple M1 —
1280x720, 3 swapchain images`, **296 frames en 5,00 s (59,2 fps)** = FIFO
v-syncé à 60 Hz. **Validation layer active, zéro erreur.**

- `platform/VulkanSurface` : seam surface (SDL `GetInstanceExtensions` +
  `CreateSurface`), handles opaques (`void*` instance, `u64` surface) → aucun
  type Vulkan dans un header (§3.1). Compilé dans `meadows-render`, comme
  `GlContext.cpp` (la moitié GL de la couche platform).
- `VulkanDevice` : instance (+ validation en debug), surface, choix du GPU
  (graphics+present+swapchain, discret préféré), device logique, swapchain
  (BGRA8 UNORM, FIFO), render pass, framebuffers, command pool, et la
  synchronisation `kFramesInFlight = 2`.
- `beginFrame`/`endFrame` : acquire → record → submit → present, avec
  recréation de swapchain sur `OUT_OF_DATE`/`SUBOPTIMAL` et sortie propre si
  l'acquire échoue (`frameActive`).
- `beginRenderPass` honore `RenderPassDesc::clearColor` (le reste du
  CommandBuffer reste no-op jusqu'à V4/V5).
- **`tools/vksmoke`** : harnais de bring-up (fenêtre + device + boucle de clear
  animée) — la boucle `Engine` dépend encore du `SpriteRenderer` GL et de
  l'ImGui GL (portés en V3/V6), donc V1 se valide hors `Engine`.

**Choix notables :**
- **Swapchain en `B8G8R8A8_UNORM`, pas `_SRGB`** : le pipeline couleur du moteur
  gère son propre gamma ; un backbuffer sRGB double-corrigerait.
- **Sémaphore `renderFinished` PAR IMAGE** (pas par frame-in-flight) : un
  sémaphore ne doit pas être réutilisé tant qu'un present précédent l'attend
  encore.
- **Portabilité MoltenVK** : `VK_KHR_portability_enumeration` + le flag
  d'instance (sans quoi le GPU n'est même pas énuméré), et
  `VK_KHR_portability_subset` côté device quand il est exposé.
- **`DeviceCaps` reste tout à `false`** : les systèmes du renderer se gatent
  dessus ; annoncer une capacité avant que V2/V3/V4 ne l'implémentent les
  ferait appeler des no-ops.
- Init des structs en `{}` puis `sType =` (pas `{ VK_STRUCTURE_TYPE_… }`) :
  `-Wextra` réclame sinon `pNext` (`missing field initializer`).
### V2 — Ressources (VMA) — ✅ FAIT (2026-07-18)

Auto-test `vksmoke` : **12/12 PASS**, validation layer silencieuse, présentation
toujours à ~58 fps.

- **VMA** (`GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator` v3.1.0, header-only,
  `DOWNLOAD_ONLY`) ; `VMA_IMPLEMENTATION` dans le seul TU du backend.
- **Buffers** : device-local par défaut (upload via staging + copie GPU) ;
  host-visible **persistantly mapped** quand `dynamic` ou `readback`.
  `updateBuffer`/`readBuffer` prennent automatiquement la bonne voie.
- **Textures** : tous les formats (RGBA8/SRGBA8/RGBA16F/R16F/R32F/Depth32F),
  `arrayLayers` (cascades CSM, splat), `depth>1` = **volumes 3D** (clipmap GI,
  cascades — `STORAGE_BIT`), mips. Upload par staging + transitions de layout.
  `generateMipmaps` = chaîne de `vkCmdBlitImage`.
- **Samplers** : filtres, adressage, anisotropie (si le GPU l'expose), et
  **samplers de comparaison** pour le PCF des ombres.
- **`copyBuffer`/`copyTexture`** enregistrés dans le command buffer de la frame.
- `immediateSubmit` : submit bloquant sur un pool **transient** séparé, pour que
  le staging ne réinitialise jamais le command buffer d'une frame.

**Vérification sans pipelines.** Rien ne peut encore être *dessiné* avec un
buffer ou une texture, donc la correction se prouve par **aller-retour mémoire**
(écriture → GPU → readback → `memcmp`), sur les deux chemins (mappé et
device-local), plus la copie GPU enregistrée dans une frame. Les textures se
valident par création + mips sans erreur de validation.

**Leçons :**
- `CompareFunc::Never` du RHI signifie « pas un sampler de comparaison », donc
  il mappe sur `compareEnable = FALSE`, **pas** sur `VK_COMPARE_OP_NEVER`.
- `VulkanDevice::Impl` doit être **nommable** par le CommandBuffer du backend
  (il lit les tables de ressources) : la déclaration est publique, le type
  reste incomplet hors du `.cpp`, le membre `impl` reste privé.
- Piège de diagnostic : un `GITHUB_REPOSITORY` erroné (`…-LibrariesAndTools` au
  lieu de `…-LibrariesAndSDKs`) fait répondre à GitHub un challenge d'auth sur
  un dépôt inexistant → git réclame un *username* et l'erreur ressemble à une
  panne réseau/credentials. **Vérifier l'URL avant de suspecter le réseau.**
### V3 — Shaders GLSL 460 → SPIR-V — ✅ FAIT (2026-07-18)

**Le point dur du chantier s'est révélé bien plus petit qu'annoncé.**
Auto-test `vksmoke` : **28/28 paires graphiques + 7/7 compute** compilées en
SPIR-V par le VRAI chemin (`ShaderLibrary` → `createShader` → shaderc →
`VkShaderModule`).

**Décision (le point de décision du plan est tranché) : PAS de shim, PAS de jeu
de shaders Vulkan dédié — UN SEUL corpus sert les deux backends.**

Le spike (`glslangValidator` sur les 53 shaders, avant toute ligne de C++) a
montré que le corpus était déjà écrit en GLSL moderne et explicite :
`layout(binding=)` partout, `std140/std430` explicites, attributs de sommet
déjà localisés, aucun `gl_FragColor` ni `texture2D()`. **Les 7 compute
shaders compilaient déjà sans rien changer** — le plus gros risque supposé
(GI, culling, Hi-Z) est tombé immédiatement.

Deux écarts réels, et deux seulement :
1. **Varyings sans `layout(location=)`** (38 fichiers, 104 déclarations) : GL
   les apparie par nom, SPIR-V exige des locations. Corrigé par numérotation
   dans l'ordre de déclaration — les `.vert`/`.frag` déclarent leurs varyings
   dans le même ordre, donc l'appariement est cohérent par construction.
   **Ces locations sont valides en GLSL 460 pour OpenGL aussi**, d'où le corpus
   unique.
2. **`gl_VertexID` vs `gl_VertexIndex`** (5 fichiers) : seule vraie divergence
   de dialecte. Neutralisée par `shaders/compat.glsl`
   (`MEADOWS_VERTEX_INDEX`), qui bascule sur la macro **`VULKAN` que
   shaderc/glslang prédéfinit** — donc **le backend GL n'a rien à injecter ni
   à changer**.

**Non-régression GL :** 43 fichiers de shaders modifiés dont dépend le renderer
GL 4.6, qui ne tourne PAS sur ce Mac. Vérifié en revalidant les 53 shaders en
sémantique OpenGL (`glslangValidator` sans `--target-env vulkan`) : **53/53
passent**. À confirmer visuellement au premier run GL 4.6 sur PC.

**Implémentation :** `shaderc` (API C, dylib du SDK) compile au **runtime**,
ce qui **préserve le hot-reload** de `ShaderLibrary` — un pré-cook `.spv` au
build l'aurait tué. `shaderc_combined.a` évité (archive de 1,3 Go).
`ShaderDesc::uniformBlocks`/`::samplers` sont **ignorés** côté Vulkan : ils
n'existent que pour l'assignation post-link du GL 4.1 ; les bindings sont déjà
explicites dans le corpus et SPIR-V les lit directement.

> **RISQUE ENCORE OUVERT pour V4 — collisions de bindings.** En GL, UBO,
> unités de texture et SSBO sont des espaces de nommage SÉPARÉS ; le corpus en
> profite (`binding = 0` est à la fois un UBO et un sampler dans plusieurs
> shaders). En Vulkan, **un seul espace par descriptor set**. glslang n'a rien
> signalé parce que ce n'est PAS une erreur de compilation d'un stage isolé —
> c'est un problème de *pipeline layout*, qui ne se manifestera qu'à la
> création des `VkDescriptorSetLayout` en V4. Compiler n'est pas valider :
> prévoir un remapping (par exemple par offset de plage selon le type de
> descripteur) au moment de V4.
### V4 — Pipelines / render passes / bind groups — ✅ FAIT (2026-07-19)

**Un triangle s'affiche en Vulkan sur le M1**, ~60 fps, **zéro erreur de
validation**. C'est la première fois que le chemin graphique complet
(pipeline → vertex buffer → descripteurs → draw) tourne.

Livré :
- **Dynamic rendering** : `VkRenderPass`/`VkFramebuffer` supprimés du backend.
  Les transitions de layout que le render pass faisait implicitement sont
  devenues des barrières explicites (backbuffer en `beginFrame`/`endFrame`,
  cibles offscreen en `begin`/`endRenderPass`).
- **`createPipeline`** : PSO complet (layout de sommets, blend, depth, cull,
  depthBias, wireframe, topology), créé **paresseusement** au premier
  `setPipeline` et mis en cache par jeu de formats de cible — `PipelineDesc`
  ne dit pas sur quoi il dessinera. `createComputePipeline` immédiat.
- **`createFramebuffer`** : une image view par attache (mip + layer
  sélectionnables), plus les formats ; aucun objet Vulkan de framebuffer.
- **`createBindGroup`** : rien à allouer — le bind group EST la liste de
  writes, poussée via `vkCmdPushDescriptorSetKHR` avec le layout du pipeline
  courant, en réappliquant les offsets de binding de V4a.
- **Draws** : `setVertexBuffer`/`setIndexBuffer`, `draw`/`drawIndexed`
  (`firstInstance` natif), `dispatch`, `memoryBarrier`, viewport/scissor.

**Conventions de repère :**
- **Y-flip** par **viewport à hauteur négative** — absorbé dans le backend, ni
  les shaders partagés ni le gameplay n'en savent rien. Le winding étant
  miroité par ce flip, `frontFace` est **inversé** à la construction du
  pipeline pour préserver le sens GL de `FrontFace`.
- **`setFrontFace` = variante de pipeline, pas d'état dynamique** :
  `vkCmdSetFrontFace` est du cœur Vulkan 1.3 (ou extended_dynamic_state) et on
  cible 1.2. Le winding entre donc dans la clé du cache de pipelines, ce qui
  ne coûte rien puisque ce cache existe déjà.
- ⚠️ **Profondeur 0..1 PAS ENCORE traitée.** GL projette en -1..1, Vulkan
  attend 0..1. Les matrices de projection viennent du renderer (GLM), pas du
  backend. À corriger au branchement du vrai renderer (V7) par une correction
  clip-space centralisée — surtout **pas** `GLM_FORCE_DEPTH_ZERO_TO_ONE`
  global, qui casserait le backend GL.

**Limite connue — pas de deletion queue.** Détruire une ressource encore
référencée par un command buffer en vol est indéfini (la validation l'a
signalé au premier essai) ; OpenGL masquait le problème en différant en
interne. En attendant une vraie file de destruction (détruire après
`kFramesInFlight` frames), les `destroy*` **drainent le GPU**
(`vkDeviceWaitIdle`). Les destructions sont rares — hot reload de shader,
éviction de texture, redimensionnement — jamais par frame, donc le stall est
acceptable pour le bring-up.

### V4 (historique) — le remap de bindings

**Fait : le remap de bindings + la réflexion** (le risque laissé ouvert par V3
est résolu).

Le risque était réel, mesuré : **11 shaders** utilisent un même numéro de
binding pour deux classes de descripteurs dans le même stage (typiquement
`binding = 0` à la fois UBO et sampler). En GL ce sont des espaces de nommage
séparés ; en Vulkan, un seul par descriptor set.

Chemins écartés, et pourquoi :
- **Les « binding shifts » de glslang/shaderc** (`--shift-sampler-binding`…) :
  testés en ligne de commande, **sans effet sur les bindings explicites** —
  ils ne servent qu'à l'auto-assignation. Sortie SPIR-V identique.
- **Un descriptor set par classe** (`set = N`) : casserait le modèle du RHI, où
  un `BindGroup` mélange les classes et doit rester **un seul** descriptor set.
  De plus `set=` n'est pas de la syntaxe GLSL valide côté OpenGL.

**Retenu : décalage par classe**, appliqué à la source GLSL dans le backend
Vulkan uniquement (UBO +0, sampler +16, SSBO +32, image +48 ; plages de 16, le
corpus culmine à UBO 5 / sampler 11 / SSBO 3 / image 0). Les mêmes offsets
seront appliqués à la construction des descriptor sets, donc **les appelants
gardent les numéros GL et ne voient rien**. Le backend GL n'est pas touché.

Bénéfice : en parsant la source pour remapper, on obtient la **réflexion
gratuitement** (classe + binding par ressource), ce dont `createPipeline` a
besoin pour son `VkPipelineLayout` — sans dépendance de réflexion SPIR-V.

Garde : après remap, `createShader` **échoue bruyamment** si deux ressources
partagent un binding, plutôt que de laisser le défaut surgir bien plus tard à
la création d'un descriptor set layout invalide.

**Reste V4** : `createFramebuffer`, `createPipeline`/`createComputePipeline`,
`createBindGroup`, l'enregistrement des draws, et les conventions de repère
(Y-flip par viewport à hauteur négative, profondeur 0..1).

#### Conception arrêtée pour la suite de V4 (extensions VÉRIFIÉES sur le M1)

Sonde écrite et exécutée sur l'Apple M1 — MoltenVK expose **les deux** :
`VK_KHR_dynamic_rendering` et `VK_KHR_push_descriptor`. Cela tranche deux
problèmes de conception qui, sinon, coûtaient cher en machinerie.

1. **`VK_KHR_dynamic_rendering` → abandonner les render passes classiques.**
   Le plan prévoyait `VkRenderPass` + `VkFramebuffer` « pour la portabilité
   MoltenVK » ; la sonde montre que l'hypothèse est caduque. Avec le dynamic
   rendering il n'y a **ni objet render pass ni framebuffer** :
   `vkCmdBeginRendering` prend directement les image views et les load/store
   ops. Cela **supprime** le cache de render passes (variantes par formats ×
   load ops) et le problème de compatibilité render-pass/pipeline.
   `createFramebuffer` se réduit à mémoriser des vues + formats.
2. **`VK_KHR_push_descriptor` → régler le décalage bind group / pipeline.**
   Vulkan veut qu'un descriptor set soit alloué depuis un layout compatible
   avec celui du pipeline, or `BindGroupDesc` ne référence aucun shader et
   `PipelineDesc` aucune cible. Avec les push descriptors, `setBindGroup`
   **pousse les writes** en utilisant le layout du pipeline courant : plus de
   set pré-alloué, plus d'appariement de layouts, et le modèle « bind group =
   simple liste d'entrées » du RHI est respecté tel quel.
   (Garde-fou : `maxPushDescriptors` — largement au-dessus de nos comptes.)

Reste que **`createPipeline` a besoin des formats de la cible** (via
`VkPipelineRenderingCreateInfo`), que `PipelineDesc` ne porte pas → créer le
`VkPipeline` **paresseusement** au premier `setPipeline`, mis en cache par jeu
de formats de la cible courante.

> Ces deux extensions sont **optionnelles** dans Vulkan : les demander à la
> création du device et **échouer proprement** si absentes (un GPU PC récent
> les a toutes deux ; `dynamic_rendering` est même cœur en Vulkan 1.3).
### V5 — CommandBuffer recording — À FAIRE
### V6 — Fences / timestamps / ImGui Vulkan / nativeTextureId — À FAIRE
### V7 — Bring-up LandscapeScene sur M1 + parité GL46 — À FAIRE
