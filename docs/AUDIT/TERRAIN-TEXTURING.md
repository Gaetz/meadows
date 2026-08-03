# Terrain texturing — brief d'implémentation

Document de travail pour une instance Claude Code opérant sur `~/Code/Engines/meadows`.
Cible : backend Vulkan, moteur custom, développeur solo.

Objectif : obtenir un terrain extérieur de qualité AAA en n'utilisant que des textures
tuilées de 512² (rarement 1024²), partagées par l'ensemble du monde.

---

## 0. Phase de reconnaissance — à faire avant toute écriture de code

Ce document décrit une architecture, pas une API. Avant d'implémenter quoi que ce soit,
lire le code existant et remplir les blancs ci-dessous. **Ne pas inventer d'API RHI.**

À inspecter et à rapporter :

- L'interface RHI : comment sont exprimés les *bind groups*, les PSO, les command buffers.
  Le vocabulaire du document (« descriptor set », « pipeline ») doit être traduit dans le
  vocabulaire réel de l'abstraction.
- Existe-t-il déjà un système de terrain (heightfield, quadtree, patchs, LOD) ? Si oui,
  ce brief ne concerne que la couche *matériau*, pas la géométrie.
- Le pipeline d'assets : y a-t-il une étape de build offline pour les textures, ou tout
  est-il chargé brut au runtime ? La compression BC est **obligatoirement** offline.
- Les features Vulkan actuellement demandées au device (`VkPhysicalDeviceFeatures`).
  On aura besoin de `textureCompressionBC`, `samplerAnisotropy`, et à terme
  `tessellationShader`.
- La convention de handedness et d'espace tangent utilisée par le reste du moteur.

Rendre compte de ces points **avant** de proposer un plan d'implémentation.

---

## 1. Le principe fondateur : densité de texels

L'intuition « petite texture = peu de détail » est fausse sur un terrain. Ce qui compte
est la densité de texels par mètre de surface monde, pas la résolution du fichier.

| Approche | Calcul | Densité |
|---|---|---|
| Texture unique 4096² sur un patch de 512 m | 4096 / 512 | **8 texels/m** |
| Texture tuilée 512² répétée tous les 2 m | 512 / 2 | **256 texels/m** |

Facteur 32 en faveur de la petite texture, pour 1/64 de la mémoire.

Conséquences directes :

- Sur un monde de plusieurs km², le texturage unique est mathématiquement impossible.
  Le tuilage n'est pas un compromis, c'est la seule option viable.
- Une fois qu'on tuile, monter au-delà de 512² ne sert à rien : on est déjà très
  au-dessus du besoin réel en texels/m. **Ne jamais autoriser de matériau de terrain
  au-delà de 1024².**
- Ce qu'on perd en tuilant, c'est l'*unicité*, pas la finesse. Tout le travail consiste
  donc à réinjecter l'unicité par d'autres canaux, en basse fréquence.

---

## 2. Décomposition en bandes de fréquence spatiale

La règle : **aucune bande de fréquence ne doit être stockée deux fois.** Les données
uniques au monde sont en très basse résolution ; les données à haute fréquence sont
tuilées et partagées.

| Bande | Contenu | Nature | Densité cible |
|---|---|---|---|
| ~1 km | Carte de teinte macro (RGB) | unique par région | ~0,5 texel/m |
| ~100 m | Splatmap / poids de couches | unique par région | 1–2 texels/m |
| ~2 m | Matériaux PBR tuilés 512² | bibliothèque partagée | ~250 texels/m |
| ~20 cm | Detail normal + roughness | bibliothèque partagée | tuilé à 10–20 cm |
| ~1 cm | Displacement (POM / tessellation) | dérivé de la height | aucun texel |

C'est cette structure qui permet « des 512 pour tout le jeu » : la bibliothèque de
matériaux tuilés (40 à 60 matériaux) est réutilisée partout, quelques centaines de Mo
en BC7 pour la planète entière. Les données uniques par région sont en basse résolution
et donc négligeables en mémoire.

**La variation artistique vit dans les basses fréquences, le détail dans les hautes.**

---

## 3. Pipeline d'assets (offline, obligatoire)

Sources recommandées, toutes en CC0 (aucune contrainte d'attribution ou de
redistribution, ce qui compte quand on embarque les assets dans un build) :

- **Poly Haven** — `polyhaven.com/textures`, jusqu'en 8K, fournit aussi les HDRI pour l'IBL
- **ambientCG** — `ambientcg.com`, catalogue plus large sur les sols naturels, API JSON
  pour scripter les téléchargements par lots
- **TextureCan**, **cgbookcase** en complément

Megascans n'est plus une option gratuite : l'accès libre s'est terminé fin 2024, les
assets sont désormais vendus à l'unité ou via les paliers d'abonnement Fab.

### Étapes de conversion

1. **Downsample en 512²** (Lanczos ou Kaiser, pas de box filter).
2. **Convention de normales** : Poly Haven et ambientCG livrent en convention OpenGL
   (+Y). Prévoir malgré tout un flag par matériau, les exceptions existent.
3. **Channel packing** : fusionner AO / Roughness / Metallic dans un seul RGB (ORM).
   Divise par trois le nombre de fetches et de bindings.
4. **Génération des mips offline.** Impératif : `vkCmdBlitImage` ne fonctionne pas sur
   une image compressée. Générer la chaîne complète en non compressé, puis compresser
   chaque niveau.
5. **Compression** :

| Map | Format Vulkan | Note |
|---|---|---|
| Albedo | `VK_FORMAT_BC7_SRGB_BLOCK` | sRGB uniquement ici |
| ORM | `VK_FORMAT_BC7_UNORM_BLOCK` | linéaire |
| Normal | `VK_FORMAT_BC5_UNORM_BLOCK` | 2 canaux, reconstruire Z dans le shader |
| Height | `VK_FORMAT_R16_UNORM` | **non compressée** — voir ci-dessous |

**Point critique : ne pas packer la height dans l'ORM en BC7.** Le blending par hauteur
compare des gradients ; 8 bits (256 niveaux) transforment la zone de transition en
bandes de contour visibles, et les artefacts de bloc du BC7 ajoutent du bruit en escalier
sur les silhouettes de POM. La height reste en R16 séparée. C'est le seul poste où
dépenser de la mémoire est justifié.

Outils : `bc7enc_rdo` ou `ISPCTextureCompressor` pour l'encodage, `stb_image` pour la
lecture. Sortie dans un conteneur KTX2 ou un format maison, chargé directement en
`VkImage` sans transcodage runtime.

---

## 4. Layout GPU

Une `VkImage` de type `VK_IMAGE_TYPE_2D` avec `arrayLayers = N` par canal :

```
albedoArray  : BC7_SRGB,  512x512, N layers
normalArray  : BC5_UNORM, 512x512, N layers
ormArray     : BC7_UNORM, 512x512, N layers
heightArray  : R16_UNORM, 512x512, N layers
```

`N` = nombre de matériaux de la bibliothèque. Toutes les couches partagent la même
résolution, ce qui rend l'array trivial. Le splatmap indexe dans cet array.

Quatre `combined image sampler` par draw de terrain, plus le splatmap et la teinte macro,
soit six bindings au total. Pas besoin de descriptor indexing à ce stade — y venir
seulement si la bibliothèque devient hétérogène en résolution.

Sampler : filtrage trilinéaire, `anisotropyEnable = VK_TRUE`, `maxAnisotropy` à 8 ou 16.
L'anisotropie est ce qui sauve les surfaces vues en rasance ; sans elle, le terrain
devient une bouillie à mi-distance quelle que soit la qualité des textures.

---

## 5. Ordre d'implémentation

Cinq phases, classées par rapport qualité/effort décroissant. **Valider chaque phase
visuellement avant de passer à la suivante.** Ne pas tout écrire d'un coup.

### Phase 1 — Splat 4 couches + blending par hauteur

Le plus gros gain visuel du document, environ une journée de travail.

Un `lerp(a, b, w)` classique produit un fondu savonneux entre matériaux. Le blending par
hauteur fait percer les graviers à travers le sable dans les creux :

```glsl
// Blend de N couches pondérées, piloté par les heightmaps.
// depth ≈ 0.1 à 0.2 : plus la valeur est basse, plus la transition est franche.
float blendHeights(float h[N], float w[N], float depth, out float b[N]) {
    float maxH = 0.0;
    for (int i = 0; i < N; ++i) {
        h[i] += w[i];
        maxH = max(maxH, h[i]);
    }
    maxH -= depth;
    float sum = 0.0;
    for (int i = 0; i < N; ++i) {
        b[i] = max(h[i] - maxH, 0.0);
        sum += b[i];
    }
    return sum;
}
```

Puis pondérer albedo, normal et ORM par `b[i] / sum`.

Critère d'acceptation : à l'interface entre deux matériaux, on doit voir les micro-reliefs
de l'un émerger dans les creux de l'autre, pas un dégradé lisse. Vérifier aussi
l'absence de banding — s'il y en a, la height n'est pas en 16 bits.

### Phase 2 — Carte de teinte macro

Une heure de travail, gain disproportionné. Un simple `albedo *= tint` où `tint` provient
d'une texture 1024² par km², échantillonnée en UV monde.

Supprime à elle seule l'essentiel de la sensation de tapisserie vue de loin, parce que la
répétition est perçue par le système visuel comme une régularité de *couleur*, pas de
détail. C'est aussi le canal principal de direction artistique : gradients d'aridité,
zones d'usure, transitions de biome.

Prévoir un facteur de force paramétrable ; au-delà de ~0,4 en intensité, la teinte
commence à écraser la variation naturelle des matériaux.

### Phase 3 — Detail normal + roughness

Une texture de détail unique, partagée, tuilée tous les 10–20 cm, mixée par-dessus les
normales de couche en *whiteout blending* :

```glsl
vec3 blendWhiteout(vec3 base, vec3 detail) {
    return normalize(vec3(base.xy + detail.xy, base.z * detail.z));
}
```

Rôle : casser le flou de mip à très courte distance, là où le matériau tuilé à 2 m
atteint sa limite de résolution. Faire disparaître le detail sur la distance avec un fade
basé sur la profondeur, sinon il produit de l'aliasing en arrière-plan.

### Phase 4 — Parallax occlusion mapping

C'est le vrai différenciateur entre « 512 qui fait cheap » et « 512 qui fait AAA ». Le
détail perçu à courte distance vient de la parallaxe et de l'auto-occultation, pas des
texels. Une 512² albedo + 512² height avec du POM bat une 4K plate, sans discussion.

Appliquer sur la couche dominante uniquement (celle de poids `b[i]` maximal), avec fade
sur la distance.

**Piège Vulkan/GLSL à ne pas manquer** : dans la boucle de ray-marching, les dérivées
implicites de `texture()` sont indéfinies (flux non uniforme entre invocations du quad).
Calculer les dérivées **avant** la boucle et utiliser `textureGrad()` à l'intérieur :

```glsl
vec2 dx = dFdx(uv);
vec2 dy = dFdy(uv);
// ... puis dans la boucle :
float h = textureGrad(heightArray, vec3(uv, layer), dx, dy).r;
```

Sans ça : bandes de mip visibles et scintillement sur les bords de triangle.

Option ultérieure : tessellation matérielle du patch proche avec displacement en vertex,
qui donne une vraie silhouette. Coût géométrique réel et gestion du cracking entre
patches adjacents (les facteurs de tessellation des arêtes partagées doivent être
identiques, calculés depuis les positions monde et non depuis l'indice de patch).
**Ne pas s'y engager avant que le POM soit stable.**

### Phase 5 — Tuilage stochastique

À faire en dernier, quand tout le reste tient. Trois échantillons avec offsets et
rotations pseudo-aléatoires, mélangés en préservant l'histogramme (variance du blend).

C'est la seule technique qui élimine réellement la répétition, mais elle triple le coût
de sampling. La réserver à la ou les couches les plus visibles.

Alternative bien moins chère si le budget ne suit pas : échantillonner la même texture à
deux fréquences non harmoniques (1× et 0,37×) et combiner en overlay. Deux fetches au
lieu d'un, la grille régulière disparaît, la variation d'histogramme est imparfaite mais
acceptable.

Même contrainte de dérivées qu'en phase 4 : `textureGrad()` obligatoire, les UV décalés
aléatoirement ont des dérivées discontinues.

---

## 6. Plus tard : cache de composition

À n'envisager que si le profiling montre que le blend multicouche domine le coût.

Avec 4 à 8 couches × 4 maps, on monte à 20–30 fetches par pixel. La parade classique
consiste à composer le résultat une fois dans un cache — clipmap ou virtual texture —
puis à n'échantillonner qu'une seule texture au rendu final.

Le displacement, lui, reste appliqué par-dessus le cache puisqu'il est dépendant du
point de vue et ne peut pas être bakké.

Coût d'entrée élevé : gestion de pages, feedback buffer, streaming asynchrone.
Sur un projet solo, c'est un chantier de plusieurs semaines. **Ne pas le lancer sans
mesure préalable démontrant que c'est le bottleneck.**

---

## 7. Pièges recensés

- Height en 8 bits → banding de contour aux transitions. Toujours R16.
- Mips générées au runtime sur format BC → impossible, `vkCmdBlitImage` ne le supporte pas.
- `texture()` dans une boucle POM → dérivées indéfinies, bandes de mip.
- sRGB appliqué à la normal ou à l'ORM → matériaux délavés et roughness fausse.
  Seul l'albedo est en sRGB.
- Anisotropie oubliée → terrain flou à mi-distance quelle que soit la résolution.
- Detail normal sans fade sur la distance → aliasing en arrière-plan.
- Teinte macro trop forte → écrase la variation naturelle, tout devient monochrome.
- Facteurs de tessellation calculés par patch et non par arête monde → cracks visibles.

---

## 8. Lectures de référence

Par ordre de pertinence pour ce chantier.

**Etienne Carrier — *Large Scale Terrain Rendering in Call of Duty*** (SIGGRAPH Advances
2023). Le plus complet et le plus récent. Décrit le Multi-Layered Terrain Material
(deux matériaux d'entrée + seuil, avec reveal map), le quadtree de patchs, le mesh
simplifier qui n'émet que X et Y en laissant Z dérivé, et l'intégration avec l'Adaptive
Virtual Texturing. Si une seule lecture, celle-ci. Sert directement de référence pour les
phases 1 et 6.

**Michal Drobot — *Quadtree Displacement Mapping with Height Blending*** (GDC 2010).
Littéralement le sujet de ce document : comment de petites textures multicouches
produisent une surface très détaillée. Le blending par hauteur de la phase 1 et
l'accélération quadtree du ray-marching de la phase 4 viennent de là. À lire avant
d'écrire le POM.

**Johan Andersson — *Terrain Rendering in Frostbite using Procedural Shader Splatting***
(SIGGRAPH 2007). Ancien mais fondateur. Pose le principe « compute instead of store » et
la séparation detail maps tuilées / masques uniques qui structure tout ce document.
Utile pour comprendre *pourquoi* l'architecture est celle-là, plus que pour le code.

**Heitz & Neyret — *High-Performance By-Example Noise using a Histogram-Preserving
Blending Operator*** (HPG 2018). La technique de référence pour la phase 5. Voir aussi la
variante hexagonale simplifiée de Morten Mikkelsen, plus facile à implémenter pour un
résultat proche.

**Ka Chen — *Adaptive Virtual Texturing in Far Cry 4*** (GDC 2015). Pour la phase 6
uniquement. Décrit la gestion de pages et le feedback buffer.

**Andrey Mishkinis — *Advanced Terrain Texture Splatting*** (Gamasutra). Court, pratique,
donne la version minimale du blending par hauteur. Bon point de départ si le papier de
Drobot paraît trop dense.

---

## 9. Note sur la référence Crimson Desert

Ce document a été motivé par l'observation que Crimson Desert utilise des textures
tuilées de 512² (rarement 1024²) pour l'ensemble du jeu.

Précision importante : **Pearl Abyss n'a jamais publié de talk technique détaillé sur le
terrain du BlackSpace Engine.** La session GDC 2025 était à huis clos sans slides
publics. Les seuls indices publics sont indirects :

- La consommation VRAM est très basse pour un jeu moderne (~4 Go au minimum, 6–8 Go en
  preset Cinematic), ce que les analyses techniques relient explicitement au manque de
  netteté perçue des textures.
- Le réglage Texture Quality contrôle la résolution générale *mais aussi* les détails de
  matériau et le parallaxe ; descendre en Low supprime des *detail textures* pour ~0,5 Go.
  Cela confirme l'existence d'une couche de détail séparée (phase 3).
- Digital Foundry souligne un displacement mapping à une échelle inédite, servant à
  simuler la profondeur à l'intérieur des textures. C'est la confirmation la plus directe
  que le détail perçu vient de la parallaxe et non des texels (phase 4).

L'architecture décrite ici est donc une reconstruction plausible à partir de techniques
publiées ailleurs, pas une description du moteur de Pearl Abyss. Elle ne doit pas être
présentée comme telle.
