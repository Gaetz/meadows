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
### V3 — Shaders GLSL 460 → SPIR-V (point dur) — À FAIRE
### V4 — Pipelines / render passes / bind groups (Y-flip, depth 0..1) — À FAIRE
### V5 — CommandBuffer recording — À FAIRE
### V6 — Fences / timestamps / ImGui Vulkan / nativeTextureId — À FAIRE
### V7 — Bring-up LandscapeScene sur M1 + parité GL46 — À FAIRE
