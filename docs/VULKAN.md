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
- **Profondeur 0..1 : traitée en V6e** — et par le mécanisme que cette note
  déconseillait. `GLM_FORCE_DEPTH_ZERO_TO_ONE` global « casserait GL » *sans
  clip control* ; **avec** `glClipControl(GL_ZERO_TO_ONE)` (GL 4.5+) il ne
  casse rien : GL 4.6 adopte la convention Vulkan. Voir V6e.

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
### V6 — Fences / timestamps / ImGui Vulkan — 🔨 MOITIÉ FAITE (2026-07-19)

**Fait : marqueurs GPU + compute de bout en bout + `DeviceCaps` activées.**

- **Fences** : `insertFence` fait un **submit vide**, que la spec ne signale
  qu'une fois tout le travail déjà soumis terminé — précisément la sémantique
  « marqueur après tout ce qui précède » du RHI. `fenceReady` **sonde** via
  `vkGetFenceStatus` et libère le handle (usage unique) ; il ne bloque jamais
  (règle de la file de complétion, Phase 5).
- **Timestamps** : `VkQueryPool`, une **région par frame-in-flight**.
  `vkCmdResetQueryPool` étant interdit dans un render pass, une région ne peut
  être réinitialisée qu'en `beginFrame` — moment où ses résultats ont deux
  frames et sont déjà lus ou abandonnés. Une **génération par région** fait
  qu'un handle périmé est reconnu comme tel au lieu de lire le résultat d'une
  autre frame. Conversion par `timestampPeriod`.
- **Compute vérifié de bout en bout** : dispatch qui élève 256 valeurs au carré
  dans un SSBO, relu et comparé par le CPU.
- **`DeviceCaps` activées en bloc** (elles étaient volontairement toutes
  fausses depuis V1) : offscreen, arrays, HDR, samplers, mipmaps, copy,
  **compute**, volumes, et `timerQueries` selon le support réel de la queue.

**Leçon de test** : le premier essai sondait la fence dans une boucle serrée de
10 000 itérations — épuisées en ~7 ms alors qu'une frame v-syncée en demande
~16. L'implémentation était bonne, **le test était faux** ; il sonde désormais
au fil des frames, ce qui est aussi l'usage réel.

**Reste V6 : le port ImGui Vulkan** — et il porte une vraie décision
d'architecture, à trancher avec le dev (voir ci-dessous).

#### ⚡ L'UI IN-GAME MARCHE DÉJÀ EN VULKAN (vérifié 2026-07-19)

Question du dev : « on n'utilise pas seulement ImGui, il y a aussi l'éditeur
nodal et l'interface in-game — quelle est la meilleure solution pour les
trois ? » Vérification faite, **ce ne sont pas trois problèmes** :

1. **RmlUi (UI in-game) — DÉJÀ agnostique du backend.** `engine/ui/
   UiSystem.cpp` implémente `RhiRenderInterface : public Rml::RenderInterface`
   entièrement sur `rhi::`. **Testé sur Vulkan** avec un vrai écran du jeu
   (`main-menu.rml`) : create + loadFont + showDocument + render, **zéro
   ligne de code spécifique au backend, zéro erreur de validation**. Le
   harnais le rend maintenant dans la boucle d'affichage.
2. **imgui-node-editor — rien à faire de spécifique.** C'est une extension
   ImGui vendorisée : elle émet des `ImDrawList`. Ce qui rend ImGui rend
   l'éditeur nodal.
3. **ImGui (panneaux dev) — le seul restant**, encore sur
   `imgui_impl_opengl3`.

**Conséquence : écrire le renderer ImGui sur le RHI, sans hésiter.** Le
comparatif « rapide mais perce l'abstraction » / « propre mais long et
risqué » était mal posé : ce n'est pas une invention mais une **copie d'un
seam éprouvé** (§2.11 « reuse before build »). `RhiRenderInterface` fait déjà
exactement la même chose — buffers sommets/indices, un pipeline, textures,
scissor, premultiplied alpha — et ImGui produit la même forme de données. Un
seul motif de rendu d'UI dans le moteur, `imgui_impl_opengl3` ET
`imgui_impl_vulkan` supprimés, aucune trappe native, §3.1 intacte.

> Preuve indirecte importante : que RmlUi — une UI complète, avec polices,
> textures, scissor et transparence — passe sur Vulkan sans modification
> valide le RHI de bout en bout. Le même chemin portera ImGui.

#### Décision tranchée — comment rendre ImGui en Vulkan

`imgui_impl_vulkan` exige les handles Vulkan bruts (instance, physical device,
device, queue, descriptor pool). Or le RHI les cache derrière un pimpl (§3.1).
Deux voies :

1. **Trappe d'accès native** : exposer une petite structure de handles réservée
   à `ImGuiLayer`, dans l'esprit de `nativeTextureId` qui existe déjà pour ce
   motif. Rapide, mais **perce l'abstraction** — le CMake assume déjà une
   « exception dev-UI GL », donc ce ne serait pas un précédent nouveau.
2. **Renderer ImGui écrit sur le RHI** : ImGui produit des listes de sommets/
   indices avec scissor et une texture — tout est déjà exprimable avec V2-V4.
   Cela remplacerait `imgui_impl_opengl3` **et** `imgui_impl_vulkan` par un
   seul chemin servant les deux backends, sans rien percer. Plus de travail, et
   un risque de régression sur l'UI dev GL qui marche aujourd'hui.
#### V6b — Push constants (bug d'alignement de l'UI) — FAIT

**Symptôme.** À l'écran, les éléments de l'UI in-game apparaissaient tous, bien
texturés, mais empilés au mauvais endroit. Le self-test ne le voyait pas : il
prouvait l'absence d'erreur de validation, pas la justesse des pixels.

**Cause — une hypothèse GL cachée dans du code prétendument agnostique.**
`RhiRenderInterface::RenderGeometry` faisait, par élément :

```cpp
device->updateBuffer(ubo, &uniforms, ...);  // translation de CET élément
cmd->drawIndexed(...);
```

En GL le flux est immédiat et dans l'ordre : chaque draw voit la valeur écrite
avant lui. En Vulkan `updateBuffer` est un `memcpy` **à l'enregistrement**,
alors que les draws s'exécutent à la soumission — tous lisent le contenu
**final** du buffer. Chaque élément héritait donc de la translation du dernier.

**Deux hypothèses écartées avant de trouver** (les deux « évidentes », les deux
fausses — d'où leur mention) : le facteur Retina (`currentExtent` vaut bien
1280x720, pas de HiDPI sans `SDL_WINDOW_HIGH_PIXEL_DENSITY`) et le scissor
(`UiSystem` passe en origine bas-gauche, le backend re-flippe — cohérent).

**Correctif — les push constants entrent dans le RHI**, le mécanisme fait pour
les constantes par draw :

- `PipelineDesc::pushConstantSize` (Vulkan exige la plage au *pipeline layout*,
  d'où un état de pipeline et non un argument de draw ; ≤ 128 octets = le
  minimum garanti par toute implémentation Vulkan).
- `CommandBuffer::setPushConstants(data, size, offset)`.
- **Vulkan** : `VkPushConstantRange` (`ALL_GRAPHICS`) + `vkCmdPushConstants` —
  la valeur est capturée *dans* le flux de commandes, donc elle colle aux draws
  qui suivent.
- **GL 4.6 et 4.1** : pas d'équivalent → bloc uniforme réservé à
  `rhi::kPushConstantBinding` (15), mis à jour via `updateBuffer`. Comme
  celui-ci est déjà virtuel, **les deux backends GL sont servis d'un coup**.
  Vérifié : aucune collision, les bindings 10/11 déjà pris sont des *samplers*.
- **Shaders** : macro `MEADOWS_PUSH_CONSTANTS(Name)` dans `compat.glsl` —
  `layout(push_constant)` en Vulkan, `layout(std140, binding = 15)` en GL. Un
  seul corpus, écriture identique des deux côtés.

**Vérification.** vksmoke : 0 erreur de validation, tous les PASS. `ui.vert`
compilé par `glslangValidator` sous sémantique **GL** *et* **Vulkan**. Build
complet vert, suite headless 516/516. La non-régression GL reste à confirmer
visuellement sur PC (impossible ici : `ui.vert` est en `#version 460`, le M1
plafonne à 4.1).

**Leçon pour la suite — plus large que l'UI.** `updateBuffer` puis `draw` dans
la même passe est un motif que GL pardonne et que Vulkan punit *silencieusement*
(pas d'erreur de validation, juste un résultat faux). **À chasser avant V7** :
le renderer paysage a bien plus de raisons d'écrire des uniformes par draw, et
il échouerait de la même façon, en beaucoup moins lisible.

#### V6c — Chasse aux `updateBuffer` par draw — FAIT (1 reste)

Audit des **29** appels à `updateBuffer` hors backend. Le critère qui tranche :

- **Buffer par draw / par slot** (`draw.ubo`, `slot->modelUbo`, `slot->ubo`,
  `ShadowMapper::cascadeUbos[i]`) → **sûr**, chaque draw a le sien.
- **Buffer écrit une fois par frame** (`frameUbo`, `lightsUbo`,
  `reflectionUbo`, `rainOcclusionUbo`, `GpuOcclusion`, `AnimPreviewPanel`) →
  **sûr**. Vérifié pour `frameUbo`, réécrit en deux points : aucun draw entre
  les deux.
- **Buffer PARTAGÉ réécrit entre des draws** → **cassé en Vulkan**. 3 sites.

**1. `RadianceCascades::cascadeUbo` — corrigé.** Un seul UBO réécrit par niveau
dans 3 boucles, avec un `dispatch` entre chaque : tous les dispatches auraient
lu les paramètres du DERNIER niveau. C'était déjà signalé en commentaire comme
déviation connue. → push constants (48 o), ce qui les sort aussi des 4 bind
groups. A nécessité d'**étendre les push constants au compute**
(`ComputePipelineDesc::pushConstantSize`, `stageFlags` selon le type de
pipeline).

**2. `FxRenderer::instances` — corrigé.** Un SSBO écrit pour le lot alpha, puis
réécrit pour l'additif, avec un draw entre : les deux draws auraient rendu
l'additif. → les deux lots empaquetés bout à bout dans le SSBO, écrit **une
fois**, et un push constant `uFxBase` décale l'indexation par lot.

**3. `SpriteRenderer::instanceBuffer` — CORRIGÉ en deux temps.** Même motif,
un `updateBuffer` par batch avec un `drawIndexed` entre. (a) `e7c5fcd` :
upload unique + tranche par OFFSET de vertex buffer (voir la note
ci-dessous — pas `firstInstance` finalement, l'offset couvre aussi GL 4.1) ;
(b) V8d : l'upload sorti de la passe (begin/upload/end), sinon il retombait
sur l'écriture en place et gardait la course inter-frame V7e.

**Le correctif n'est PAS celui de FxRenderer** (correction d'une première
estimation trop rapide) : les sprites ne *pull* pas depuis un SSBO, ils
reçoivent leurs données par **attributs de sommet instanciés** (locations 2-5,
divisor 1). Un push constant ne peut donc pas décaler l'indexation depuis le
shader. La vraie voie est `firstInstance`, **que le RHI expose déjà** dans
`drawIndexed` : uploader toutes les instances une fois, puis
`drawIndexed(6, count, 0, batch.firstInstance)`. Reste à traiter GL 4.1, qui
n'a pas `glDrawElementsInstancedBaseInstance` — `GlDeviceBase` porte déjà un
drapeau `baseInstance` pour ça, donc le repli existe mais est à câbler.

**Vérifié** : build complet vert, vksmoke 0 erreur de validation, headless
516/516, et les 5 shaders touchés compilés par `glslangValidator` sous
sémantique **GL et Vulkan**. La parité visuelle GL reste à confirmer sur PC.

#### V6d — Le renderer ImGui sur le RHI — FAIT (GL 4.1 dégradé)

`engine/ui/ImGuiLayer.cpp` porte désormais son propre renderer écrit sur le
RHI, calqué sur `RhiRenderInterface` (RmlUi) : pipeline + atlas de polices +
bind groups par texture + scissor, et l'aplatissement de toutes les
`ImDrawList` en UN upload par frame, chaque commande sélectionnant sa tranche
par offset de vertex buffer + firstIndex (le motif V6c — jamais de réécriture
de buffer entre les draws). `imgui_impl_opengl3` est SUPPRIMÉ du build ; seul
`imgui_impl_sdl3` reste (la moitié plateforme, légitimement SDL, §3.1).
`imgui_impl_vulkan` n'entrera jamais. imgui-node-editor est servi par le même
chemin (il n'émet que des `ImDrawList`).

**`ImTextureID` change de sens** : c'est maintenant un id de
`rhi::TextureHandle`, plus un nom GL natif. Un seul site utilisait
`nativeTextureId` pour ça (`AnimPreviewPanel`) — mis à jour ; `ImGui::Image`
devient identique sur tous les backends.

**Décision — GL 4.1 dégradé, pas supporté** (dev, 2026-07-19). Le shader ImGui
utilise `layout(binding=)` sur bloc uniforme et sampler : GLSL 420+, donc
impossible en GL 4.1 sans chirurgie (`glUniformBlockBinding` après link,
`compat.glsl` à bloc nommé fixe — la piste reste notée dans le commit
`wip/imgui-on-rhi` d'origine si un jour le low-spec 4.1 veut ses panneaux
dev). Plutôt que de retarder le chantier Vulkan : **sur GL 4.1 l'UI dev est
absente et le jeu tourne** — `ImGuiLayer::create` dégrade au lieu d'échouer
(atlas construit d'abord pour garder `NewFrame` légal ; `render()` enregistre
rien si le pipeline manque). Sur macOS l'UI dev vit sur le backend Vulkan ;
GL 4.1 reste le low-spec « jeu seul ».

**Vérifié** : vksmoke enregistre une vraie frame de widgets ImGui sur Vulkan
(`testImGui`, 0 erreur de validation) ; `true-adventurer` démarre sur M1 en
GL 4.1 avec le warning et 12 s de frames sans crash ; headless 516/516. La
parité GL 4.6 (où le shader 460 compile normalement) reste à confirmer
visuellement sur PC, comme le reste du chantier.

#### V6e — Profondeur 0..1 partout — FAIT

**Le mécanisme : UNE convention, pas un fixup par backend.**
`GLM_FORCE_DEPTH_ZERO_TO_ONE` est défini **globalement** (racine du CMake —
la macro change du code inline glm, donc chaque TU doit être d'accord : ODR) ;
le backend GL 4.6 appelle `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` à
l'init. Vulkan est natif 0..1 ; GL 4.6 est *converti* ; GL 4.1 (pas de clip
control) n'exécute jamais le 3D (shaders 460) et ses pipelines 2D n'ont aucun
état depth (vérifié). L'alternative — matrice de correction côté Vulkan —
aurait laissé deux conventions vivantes : chaque reconstruction de profondeur
dans les shaders aurait exigé un `#ifdef VULKAN`, et chaque futur site de
projection un branchement. Révision assumée de la note V4 ci-dessus (le
« pas de GLM_FORCE » était vrai sans clip control, faux avec). Bonus align
avec la cible « Vulkan renderer final » : le reverse-Z (précision depth, un
prérequis GPU-driven) devient un switch au lieu d'une migration.

**Les sites -1..1 corrigés** (l'hypothèse était disséminée, pas centralisée) :
- `worldFromDepth` ×3 (water, contactshadow, volumetric) : `depth*2-1` → `depth`.
- `shadow.glsl` / `locallights.glsl` / `volumetric.frag` : le remap `*0.5+0.5`
  ne s'applique plus qu'à **xy** (UV) ; z est déjà la profondeur fenêtre.
- `chunk_cull.comp` (Hi-Z) et `rain.vert` (occlusion pluie) : `ndc.z` direct.
- `rc_debug.frag` : near à `0.0` (au lieu de -1).
- `Frustum::fromViewProj` : plan near = `r2` seul (0 ≤ z_clip), plus `r3+r2`.
- **`obliqueProjection` re-dérivée en ZO** et sortie dans
  `engine/render/Projection.hpp` (header pur math) : sous 0..1 la nouvelle
  ligne z est directement *proportionnelle au plan* (z_clip = 0 sur le plan) —
  plus simple que la variante -1..1 (`c*2`, `+1`). Sites épargnés d'office :
  sky (`z=w` → far = 1 dans les deux conventions), godrays (`step(0.99995,d)` —
  la profondeur *fenêtre* est [0,1] quelle que soit la convention NDC), et
  toutes les conversions `*2-1` d'UV.

**Preuve headless — rien de tout ça n'est visible à l'œil sur M1.** Trois
tests ajoutés (`FrustumTest.cpp`) : glm projette near→0/far→1 (échoue si la
macro saute), le plan near du frustum cull e derrière l'œil (l'extraction
-1..1 ne l'aurait PAS cullé — le test distingue les deux), et l'oblique ZO
(z_ndc=0 exactement sur le plan, >0 au-dessus, <0 dessous, xy/w intacts).
**519/519.** vksmoke : 0 erreur de validation. `true-adventurer` GL 4.1 : OK.
Les shaders touchés + leurs consommateurs (mesh/terrain/skinned.frag, includes
expansés) compilent sous les deux sémantiques. Parité visuelle GL 4.6 sur PC :
toujours le même caveat de chantier.

### V7 — Bring-up LandscapeScene sur M1 — FAIT (2026-07-19)

**`true-adventurer` fait tourner la LandscapeScene complète sur Vulkan/M1 :
0 erreur de validation**, et le GpuProbe mesure le vrai pipeline —
`shadows=5.4 rcBuild=9.7 rcMerge=5.8 mainPass volumetric` — donc CSM, les
Radiance Cascades (compute), la passe principale et les volumétriques
s'exécutent tous. **Vulkan est le backend PAR DÉFAUT** (`EngineConfig`), avec
la chaîne §2.1 dans `Engine::init` : si Vulkan échoue ou n'est pas compilé,
la fenêtre est recréée (le flag de surface SDL est figé) et GL prend le
relais.

Le bring-up a été une remontée de couches, chacune un écart GL↔Vulkan réel.
Dans l'ordre de découverte :

1. **shaderc gate les features sur le `#version` déclaré** : les shaders 2D
   inline (sprite, 410 pour GL 4.1) refusent `binding=` même en mode Vulkan.
   → `promoteVersion()` : le backend promeut toute source à 460 avant
   compilation (SPIR-V se moque de la version GLSL). Les sources sprite sont
   passées en duale (`#ifdef VULKAN` pour les bindings, locations partout).
2. **`remapBindings` classait mal `uniform writeonly image3D`** — le
   qualificateur mémoire APRÈS `uniform` faisait classer les storage images
   du RC en uniform block → collisions de bindings. → skip des
   qualificateurs, et `find()` au lieu d'un test de préfixe (i/u-variantes).
3. **Les slots de groupe ne sont PAS des sets Vulkan** : l'espace de noms du
   RHI est le binding (offsets par classe) ; tout se pousse sur le set 0.
4. **Les push descriptors meurent au changement de pipeline** ; le contrat
   GL (les groupes survivent) est restauré par un REJEU : le command buffer
   retient les 4 slots et les re-pousse à chaque `setPipeline` (motif RC :
   frame group lié une fois, puis build/extend/merge alternent).
5. **Features du portability subset = opt-in** à la création du device
   (query `vkGetPhysicalDeviceFeatures2` → chain). MoltenVK/M1 n'offre PAS
   `mutableComparisonSamplers` → les samplers de comparaison (PCF) sont
   IMMUTABLES dans les layouts : la réflexion marque `sampler*Shadow`, un
   sampler PCF du device (linear, LESS_OR_EQUAL) est fixé à ces bindings.
6. **Layouts d'image honnêtes** : le descripteur dit `tex->layout` (les
   volumes GI vivent en GENERAL) ; transition automatique vers GENERAL au
   premier usage storage (hors passe) ; `SAMPLED_BIT` et `STORAGE_BIT`
   posés partout où le format le permet (parité GL : tout est
   échantillonnable, le compute écrit aussi des cibles 2D — Hi-Z, neige).
7. **Mini deletion queue** (la dette V4 payée où elle a mordu) : détruire
   pendant l'ENREGISTREMENT est unsafe même après un idle — les commandes
   qui référencent la ressource ne sont pas encore soumises. Les destroys
   mid-frame sont parqués et libérés quand le slot de frame recycle
   (prouvé par fence).
8. **La sémantique GL « tout binding lu est lié à QUELQUE CHOSE »** est
   restaurée par des dummies : la réflexion enregistre la dimensionnalité
   (2D/2DArray/3D/Cube) et la comparaison ; au draw/dispatch, tout binding
   déclaré que nul groupe n'a poussé reçoit un dummy de la bonne forme
   (blanc 2D/array/3D, buffer 256 o, **Depth32F pour les bindings de
   comparaison** — un RGBA8 ne supporte pas le depth-compare). Les groupes
   rejoués sur un pipeline où le numéro a un autre sens (3D vs 2D) sont
   également substitués — GL échantillonnait du garbage en silence dans
   tous ces cas.
9. **ImGui s'enregistre dans une passe Load** sur le backbuffer
   (`Engine::loop`) — GL tolérait les draws nus, Vulkan exige une passe.
10. **`ImTextureID`/sampler-less** : sampler par défaut PAR TEXTURE, créé
    depuis `TextureDesc::filter/wrap` (le contrat « les paramètres de
    création s'appliquent » que GL donnait gratuitement).

**Vérifié** : LandscapeScene 35 s sans erreur de validation ; vksmoke 30
PASS ; headless 519/519. **Piège d'outillage** : un build ninja tué en plein
FetchContent laisse le clone git de VMA corrompu (« Failed to get the hash
for HEAD ») — purger `_deps/vulkanmemoryallocator-*` et reconfigurer.

**Reste (hors bring-up)** : la parité VISUELLE se juge à l'œil (le M1 du dev
— c'est désormais possible) ; la parité GL 4.6 sur PC (caveat inchangé) ;
le GL 4.1 fallback n'est plus atteignable que si Vulkan manque.

#### V7b — Session de test M1 (2026-07-19) : clavier macOS, Y-flip, divers

- **Y-flip validé à l'écran** (axe Y bon après le fix par-cible).
- **Clavier macOS** : aucun événement clavier ne parvenait à l'app — la
  barre de menus restait sur CLion : un binaire HORS BUNDLE ne devient pas
  l'application ACTIVE, et macOS ne livre le clavier qu'à l'app active (la
  souris suit le curseur, d'où l'asymétrie trompeuse clics-OK/touches-mortes).
  `SDL_RaiseWindow` ne suffit pas, et l'activation Cocoa one-shot est
  IGNORÉE avant le premier tour de runloop (activation coopérative) ; le fix
  est `platform/macos/Activation.mm` (premier per-OS .cpp du motif §3.1) +
  RETRY à chaque pumpEvents jusqu'à `[NSApp isActive]`. F2/F3 sur MacBook =
  touches média : Fn+F2, ou l'option clavier « touches de fonction standard ».
- **Fuite de samplers à la sortie** : les defaultSampler par-texture (V7)
  n'étaient libérés que par destroyTexture, pas par le teardown du device.
- **Spam `gpu frame spike`** étranglé à ~1 ligne/5 s (chaque frame Debug/M1
  dépasse 25 ms — le panneau perf porte les chiffres vivants).
- **Bug Hi-Z (terrain lointain culler) — TROUVÉ ET CORRIGÉ** : le backend
  ignorait `BindGroupEntry::imageMip` — `pushGroup` liait la VUE COMPLÈTE de
  la texture pour les storage images, donc chaque passe `hiz_down` réécrivait
  le mip 0 et les mips 1..N restaient du garbage. Les chunks lointains
  (petite empreinte → lod élevé → mip garbage → farDepth≈0) étaient cullés ;
  les proches survivaient par la sortie « bord d'écran = visible ». GL liait
  le bon niveau via `glBindImageTexture(level)` — d'où un bug Vulkan-only.
  Fix : vues PAR MIP paresseuses (`textureMipView`), libérées par les trois
  chemins de destruction (immédiat, deletion queue, teardown). Un descripteur
  storage doit viser exactement un mip — c'est la règle Vulkan que GL rendait
  implicite.

#### V7c — VkPipelineCache persisté + revue de structure (2026-07-19)

**Cache de pipelines sur disque** (`vulkan-pipeline-cache.bin`, à côté du
binaire) : MoltenVK compile le shader Metal au PREMIER usage d'un pipeline
(10-100 ms), et les variantes étant créées paresseusement par jeu de formats
de cible, un panoramique caméra rapide pendant les premières minutes traverse
des combinaisons jamais vues — à-coups qui « guérissent » une fois tout
compilé (le symptôme « surface sombre 1-2 min » du test dev). Le cache déplace
ce coût au premier lancement. L'en-tête du blob est validé par Vulkan
(UUID driver/device) : fichier périmé = ignoré.

**Revue de structure — améliorations classées (V8+)** :
1. FAIT : cache de pipelines persisté (ci-dessus).
2. FAIT : uploads async in-frame (V7, `immediateSubmit(wait=false)`).
3. **Pré-chauffe des variantes** au chargement (compiler les paires
   pipeline×formats connues pendant l'écran de titre) — supprime même le
   coût du premier lancement.
4. **Queue de transfert dédiée** (la plupart des GPU discrets en ont une ;
   M1 n'en a qu'une famille) : uploads en vrai parallèle + ownership
   transfer — à faire quand le PC discret redevient la cible active.
5. **Enregistrement parallèle des command buffers** (secondaires ou
   multi-primaires) : prématuré — le grief V8 est le coût MoltenVK par
   frame Debug, pas la saturation d'un thread d'enregistrement.
6. **Timeline semaphores** (cœur 1.2) pour remplacer le couple
   fences/sémaphores binaires — simplification, pas un gain de perf.
7. Le fichier `VulkanDevice.cpp` (~3400 lignes) reste UN TU par choix
   (VMA_IMPLEMENTATION, types partagés) ; à découper seulement si un
   second backend (PC) le rend pénible.

#### V7d — « Surface sombre » : compilation ÉLIMINÉE, cap sur le GI

Le cache de pipelines n'a PAS éteint le symptôme (surfaces sombres
transitoires au pano rapide, vertical ou latéral) — la compilation Metal est
donc hors de cause. Hypothèse restante la plus forte : la **latence de
convergence des Radiance Cascades** — l'injection du clipmap suit la caméra
avec un budget par frame ; à ~25 ms/frame (Debug M1 MoltenVK), la vague
sombre dans la direction nouvellement révélée devient visible, là où le PC
GL à 60+ fps la masquait. À vérifier en V8 : (a) profil Release (frames plus
courtes → vague plus courte), (b) budget d'injection par frame, (c) fallback
d'éclairage pendant la convergence. Ce n'est PAS un bug de justesse Vulkan :
la validation est silencieuse et le rendu converge vers le bon résultat.

#### V7e — « Surface sombre » RÉSOLU : course CPU/GPU sur les buffers dynamiques

L'hypothèse GI de V7d était fausse — et réfutable sur plan : `prepare()`
snappe les origines des clips sur la POSITION caméra, un pano (rotation) ne
déplace donc rien côté GI. Le vrai coupable, désigné par la dichotomie du
dev (plaques disparues « Shadows » OFF, insensibles au toggle round-robin)
puis prouvé par l'expérience `kFramesInFlight = 1` (plaques éteintes) :

- **`updateBuffer` sur un buffer host-visible faisait un memcpy EN PLACE,
  immédiat**, sans versionnement par frame en vol. Avec 2 frames en vol, la
  frame N−1 s'exécute encore quand le CPU de N écrit ses UBOs : son main
  pass échantillonne ses depth maps CSM avec **les matrices de N** →
  mismatch matrice/carte → projections hors carte → clamp sur les texels de
  bord → plaques d'ombre arbitraires. Visible seulement quand les matrices
  changent fort d'une frame à l'autre (pano/translation rapides), jusqu'à
  la distance CSM, par bandes de cascade — d'où le clignotement « pas tous
  les chunks en même temps ». GL est immunisé : le driver versionne les
  `glBufferSubData` ; et la validation standard ne voit rien (il faudrait
  la couche *synchronization validation*).
- **Fix** : pendant l'enregistrement d'une frame, l'update d'un buffer
  mappé passe par un staging + `vkCmdCopyBuffer` enregistré **dans le
  command buffer de la frame courante** (`recordHostUpdate`), encadré de
  barrières : dépendance d'exécution WAR contre les lectures de la frame en
  vol (une pipeline barrier couvre tout ce qui a été soumis avant), puis
  transfer→reads pour les consommateurs de la frame. Le staging est parqué
  dans la deletion queue (motif V7). L'écriture en place reste pour
  l'init/les tools (rien en vol) et les updates EN PASSE (copie illégale) —
  concrètement les vertex streams ImGui, qui tolèrent la course (dev UI) ;
  le jour où ça gêne : scinder l'upload ImGui hors passe (deux phases).
- **Vérifié** : vksmoke PASS complet, 0 erreur de validation, présentation
  59,8 fps. Confirmation visuelle au pano : le dev.

### V8 — Parité visuelle + perfs (reste du chantier)

Reliquats connus au démarrage du chantier : parité visuelle à l'œil sur M1 ;
mesure Release (le grief V7b « 25 ms/frame » était du Debug) ;
**imposteurs végétation distante** (arbre lointain = 1 quad alpha-testé
depuis une texture bakée par variante — le levier vertex identifié en V8c ;
décision différée, dev 2026-07-19 ; en attendant, curseurs existants :
`highDetailRadius` 5, `viewRadius` arbres 12) ;
puis post-démo/PC : queue de transfert, parité GL 4.6 PC, timeline
semaphores, upload ImGui deux phases. (Faits depuis : SpriteRenderer →
V8d ; sous-probes honnêtes + compteurs de géométrie et pré-chauffe close
sans code → V8e.)

#### V8a — Synchronization validation : câblée, backend purgé — FAIT (2026-07-19)

La leçon V7e industrialisée : la validation standard ne voit AUCUNE course —
la *synchronization validation* de VVL les voit toutes. Câblage :

- **Opt-in par run** : `MEADOWS_VK_SYNC_VALIDATION=1` chaîne
  `VkValidationFeaturesEXT` (SYNC) à la création d'instance (Debug + layer
  présent). Pas un défaut permanent : ce mode coûte plusieurs ms par frame.
- **Messenger `VK_EXT_debug_utils`** : les messages de validation partent
  dans spdlog (`[vk-validation]`, ERROR/WARN) au lieu de stdout, comptés, et
  le teardown loggue le verdict du run en une ligne (« clean run » ou
  « N error(s), M warning(s) »).

L'audit a trouvé et purgé **trois classes réelles** (aucune n'était visible
à l'œil sauf indice ; toutes du même genre que V7e) :

1. **WAR sur l'image swapchain acquise** (14×/run vksmoke) : la transition
   UNDEFINED→COLOR passait par le `transitionLayout` générique, srcStage
   TOP_OF_PIPE — non chaînée à l'attente du sémaphore d'acquisition
   (`pWaitDstStageMask` = COLOR_ATTACHMENT_OUTPUT). → barrière dédiée dans
   `beginFrame`, srcStage = COLOR_ATTACHMENT_OUTPUT (la règle canonique).
2. **RAW sur les uploads device-local mid-frame** (le streaming terrain) :
   le submit async V7 (`immediateSubmit(wait=false)`) ne portait AUCUN
   ordering contre les draws de la frame qui consomment le buffer. → unifié
   sur `recordHostUpdate` (V7e) : copie enregistrée dans le command buffer
   de la frame, barrières des deux côtés, staging parqué — même propriété
   « zéro stall », ordering en plus. Le submit bloquant ne reste que pour
   l'init/les tools.
3. **`layoutMasks` trop étroits** (LandscapeScene, 2 formes) : depth =
   EARLY_FRAGMENT_TESTS seul → la transition de fin de passe CSM raçait le
   store depth (LATE) ; et les accès attachment n'incluaient pas `*_READ` →
   un `loadOp LOAD` relisait l'attachement hors du scope de la barrière.
   → EARLY|LATE et WRITE|READ sur les deux layouts attachment.

**Vérifié** : vksmoke ET LandscapeScene (60 s, boot complet) **0 hazard**
sous sync validation ; 2 warnings bénins restants (« vertex attribute at
location 2 not consumed », les casters sur layout mesh complet —
cosmétique). Workflow futur : tout nouveau chantier RHI se relit avec
`MEADOWS_VK_SYNC_VALIDATION=1 ./tools/vksmoke` + une passe de scène.

#### V8b — Le poste « shadows » : cull par cascade — FAIT (2026-07-19)

**La mesure d'abord** (HUD GPU dev, M1 Debug, monde ENTIÈREMENT streamé —
les chiffres V7 « shadows=5.4 » étaient pris avant la fin du streaming,
le régime stationnaire arrive vers la frame 300) : frame GPU ~87 ms avg,
dont shadows 37,5 (43 %), mainPass 27,5, reflection 10-21. Swapchain
réelle : **1440×797** (l'EngineConfig demande 1920×1080 mais l'écran M1
fait 1440×900 points ; pas de sur-résolution Retina, SDL est créé sans
HIGH_PIXEL_DENSITY). A/B au passage : les barrières V7e/V8a ne coûtent
rien (HEAD 111,7 ms vs backend pré-V7e 121,3 ms à la même frame).

**L'indice dev qui a tout donné : le curseur de résolution de la shadow
map ne changeait RIEN, sur M1 comme sur PC.** Le curseur fonctionne (la
texture est bien recréée) — c'est le coût qui n'est pas dans les pixels :
le CSM était **vertex-bound**, parce que chaque cascade dessinait
terrain + végétation sur tout le ring de 9 chunks Chebyshev (576 m),
**cascade 0 (0,5-45 m, ortho ~30 m de rayon) comprise** — ~99 % des
sommets payés puis clippés, ×3 cascades.

**Fix** : cull des casters contre le volume ortho DE CHAQUE cascade —
`Frustum::fromViewProj(cascades.viewProj[i])` + test AABB par chunk dans
`TerrainSystem::drawDepth` et `VegetationSystem::drawDepth` (mêmes
conventions AABB que leurs draw normaux ; le cap Chebyshev 9 reste en
préfiltre). Exact par construction : le volume ortho INCLUT le caster
reach vers le soleil (`kCasterReach` est dans la matrice), donc les
ombres portées par les montagnes lointaines survivent à tout angle.

**Résultat** (même protocole 75 s) : shadows 35-50 ms → **~5 ms**
stationnaire (÷7) ; frame spike 111,7 → **51,6 ms**. Le nouveau n°1 est
le mainPass (~24,6 ms) → brique suivante : render scale + réparation des
sous-probes (liste V8). Restes shadows connus : `drawShadowCasters`
(meshes/PNJ, toggle B2a) ne cull pas encore par cascade ; un spike
shadows ~18 ms subsiste sur les frames de re-fit complet (sun step).

#### V8c — Render scale (RETIRÉ) + verdict : le mainPass M1 est VERTEX-bound (2026-07-19)

> **RETIRÉ le jour même** (verdict dev) : aucun gain fps — la mesure
> ci-dessous a prouvé le vertex-bound — et l'upscale linéaire est jugé
> moche. Le code est reverti (le boot log « Offscreen scene target »
> reste) ; **l'acquis de la brique est la MESURE**, qui redirige la perf
> M1 vers la géométrie (→ imposteurs, liste V8). L'historique
> d'implémentation ci-dessous est conservé pour référence.

**Render scale livré** (demande dev) : les passes 3D (cible offscreen,
copies scène, pyramide Hi-Z, `uScreenInfo`) rendent à
`renderScale × fenêtre` ; le blit tonemap vers le backbuffer natif
upscale linéairement, l'UI (ImGui/RmlUi) reste native. Slider
« Render scale » (0.4-1.0) dans le panneau Rendering, motif
reflectionScale (le changement de dims déclenche la recréation). La
réflexion se scale en cascade (reflectionScale × scène). Boot log :
« Offscreen scene target: WxH » (la preuve de mesure).

**Le verdict de la mesure, plus précieux que la brique** : à 0.5
(720×399, ¼ des pixels, vérifié au boot log), mainPass 25,1 → 23,9 ms et
reflection inchangée (9,5) — **le mainPass et la réflexion sont
vertex-bound sur M1, comme l'était le CSM avant V8b**. Le render scale
reste utile (PC, futures charges fragment) mais le levier M1 est la
GÉOMÉTRIE : ~24 ms de vertex shading pour arbres pleine définition à
toute distance + brins d'herbe + chunks terrain, payés une 2e fois par
la réflexion. Pistes suivantes, par ROI : (a) végétation du mainPass sur
le jumeau low-poly au-delà d'une distance (les `lowVertexBuffer`
EXISTENT — les casters les utilisent déjà) ; (b) la réflexion en
low-twin/sans végétation ; (c) densité/distance herbe. Vérifs : scale
1.0 = chiffres identiques à V8b (52,2 ms fr. 405) ; 0 hazard, 2 warnings
cosmétiques inchangés.

#### V8d — SpriteRenderer : upload hors passe — FAIT (2026-07-19)

Le « 1 reste » V6c était en fait déjà à moitié réglé le matin même
(`e7c5fcd` : upload unique + tranche par offset de vertex buffer — GL 4.1
couvert sans `GL_ARB_base_instance`). Restait la moitié V7e : l'upload se
faisait DANS la passe sprite (SceneStack/Game ouvraient la passe avant
`end()`), donc retombait sur l'écriture en place → course inter-frame sur
`instanceBuffer` ET `cameraUbo`. Découpe begin/upload/end : `begin()` ne
fait plus que la collecte, `upload()` (nouveau) écrit les deux buffers HORS
passe via le chemin barriéré V7e, `end()` n'enregistre que les draws. Les
deux appelants (SceneStack::render, Game::render par défaut) collectent
puis uploadent AVANT `beginRenderPass`. Le renderer 2D est désormais
correct sur Vulkan au même titre que le 3D ; vérification visuelle d'une
scène 2D (CombatArena) : validée dev 2026-07-19.

#### V8e — Sous-probes honnêtes + compteurs de géométrie ; pré-chauffe close — FAIT (2026-07-19)

- **Les sous-probes du mainPass ne peuvent PAS mesurer sur Metal** : un
  GPU tuilé exécute la passe entière comme une unité (tout le vertex,
  puis le fragment par tuile) — un timestamp au milieu de l'encoder
  n'échantillonne rien (les 0,01 ms constatés). Ce n'est pas réparable,
  c'est structurel. → cap `DeviceCaps::midPassTimestamps` (GL 4.6 : true ;
  Vulkan : false) ; les scopes mainTerrain/mainVeg/mainGrass ne
  s'enregistrent que là où ils mesurent, leurs lignes disparaissent du
  panneau sur Vulkan au lieu de mentir.
- **La dissection passe par des compteurs CPU** (panneau GPU perf,
  bloc « Geometry this frame ») : indices sommés sur TOUTES les passes
  (casters, reflet, main) par système — terrain (Mtri), arbres (Mtri +
  instances high/low), herbe (Mtri + brins), total. Reset dans les
  update(), sommés dans les draw*(). C'est l'entrée de la décision
  imposteurs : le total dira qui porte les ~24 ms du mainPass.
- **Pré-chauffe des variantes (V7c-3) : CLOSE sans code.** Le cache
  VkPipelineCache persisté déplace déjà la compilation Metal au premier
  lancement seul, et l'écran titre rend le paysage complet — les
  variantes du chemin principal se compilent dans les premières frames.
  Une pré-chauffe explicite exigerait de persister les paires
  pipeline×formats à travers les runs (les handles ne sont pas stables)
  pour ne couvrir que les à-coups de premier lancement des scènes
  secondaires. À rouvrir seulement si un à-coup premier-run gêne
  réellement (test : supprimer `vulkan-pipeline-cache.bin` et jouer).

Vérifié : boot + 40 s propres (0 erreur validation), spikes inchangés.

#### V8f — Arbres : 3e niveau de LOD mesh (ultra, 20 faces/lobe) — FAIT (2026-07-19)

Les compteurs V8e ont tranché (lecture dev) : **24 des ~30 Mtri/frame
sont les arbres**, 6 l'herbe. Le levier « arbres lointains » validé par
le dev s'implémente SANS nouvelle tech (§2.11) : `appendBlob` est un
icosaèdre subdivisé récursivement, donc `generateTree(seed, 0)` existe
déjà — lobes à 20 faces, ~150 tris/arbre contre ~600 au jumeau low.
Même seed = même composition/couleurs/silhouette ; à 500 m les facettes
sont invisibles.

- `VariantMesh` gagne le trio ultra (buffers + count) ; `draw()` passe à
  TROIS niveaux : 320 faces ≤ `highDetailRadius` (5), 80 faces ≤
  `lowDetailRadius` (8, slider « Veg low-detail radius »), 20 faces
  au-delà. Variantes sans jumeaux (rochers, buissons, meshes auteurs) :
  inchangées.
- Le REFLET force l'ultra (miroir mi-res : 9,5 → 3,7 ms, ÷2,5) ; les
  CASCADES lointaines (1-2) castent en ultra, la 0 garde le low
  (shadows 5,1 → 3,2 ms typique ; 17,9 → ~9 sur les frames de re-fit).
- Compteur ultra dans le bloc géométrie du panneau perf.

**Mesure (protocole 75-100 s, M1 Debug)** : frame spike 51,6 →
**~39,5 ms** (re-fit : 63,9 → ~45,5) ; mainPass 24,6 → 20,5 ms.
**Descente dev (vérif visuelle OK, 2026-07-19)** : `lowDetailRadius`
8 → **4** (défaut, porté par `vegLowDetailRadius` appendu au
`LandscapeTuningForm` — moddable §5, suite headless 81 925/81 925) →
frame spike **37,1 ms** (re-fit ~42,7), mainPass 18,4. Bilan de la
journée : 87 → 37 ms (~10 → ~27 fps Debug M1). Si le plancher mesh ne
suffit pas, les imposteurs restent listés — le pipeline 3 niveaux leur
a préparé le branchement (un 4e niveau quad serait local à draw()).

#### V8g — Mesure Release : GPU-bound confirmé, le mur est la vsync — FAIT (2026-07-19)

Même protocole (75 s, monde streamé), binaire Release (`cmake-build-release`,
profil CLion créé le matin même) :

- **Timings GPU identiques au Debug** (~37 ms stationnaire, ~43-45 au
  re-fit, mêmes postes) — logique, le GPU exécute les mêmes shaders. Le
  grief V7b (« le coût MoltenVK par frame Debug ») est RÉFUTÉ par la
  mesure : le CPU Debug n'était pas le goulot, la frame est GPU-bound.
- **FPS réels (horodatages du log, 301 frames/ligne)** : Release
  ~21,0 fps, Debug ~22,7 — égaux au bruit près, et QUANTIFIÉS par la
  présentation FIFO (vsync) : une frame GPU de 37 ms rate le créneau de
  33,3 ms et attend celui de 50 ms (60/3 = 20 fps).
- **Conséquences** : (a) le prochain palier de fps réel est net — passer
  le GPU sous 33,3 ms (→ 30 fps vsync) : il manque ~4 ms, le mainPass
  (18,4) reste le poste ; (b) pour des MESURES non quantifiées, un
  présent-mode IMMEDIATE optionnel (knob dev) serait utile — brique
  facultative ; (c) inutile d'attendre de la perf du profil Release côté
  rendu — il servira au CPU (sim, streaming, bakes AO).

