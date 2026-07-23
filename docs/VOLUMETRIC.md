# VOLUMETRIC — le brouillard comme éclairage, extérieur & intérieurs

> Spec du chantier « lighting volumétrique » : pourquoi le height fog actuel
> grise l'image, pourquoi la réponse n'est PAS le raytracing, comment le
> volumétrique s'articule avec la GI Radiance Cascades, et le plan de
> briques — extérieur (V1→V4) puis intérieurs façon « Helios » (H1→H4).
> Style : chaque brique est livrable seule ; la suivante est optionnelle
> et se décide à la mesure (panneau F6). Rien ici n'exige de nouveau
> mécanisme moteur : tout étend `volumetric.frag`, `LightForm`, la CSM,
> la météo (`WeatherForm`) et l'horloge (§2.11).

## 1. Le constat, et la théorie utile

La lumière qu'un rayon de vue récolte en traversant l'air se décompose en
deux termes d'**in-scatter** :

- l'**ambient** — l'air renvoie la lumière du ciel/de l'environnement ;
- le **solaire** — `sunColor × phase(view·sun) × visibilité(p)`, intégré le
  long du rayon, où la *visibilité* est l'ombre portée (CSM + nuages).

Le fog actuel (`applyFog`, `engine/render/landscape/shaders/sky.glsl`) fait
`mix(color, skyGradient(viewDir), amount)` : extinction exponentielle par
la hauteur, teintée par le ciel directionnel (halo solaire compris). C'est
le terme ambient seul. Sans le terme solaire, l'air éclairé et l'air à
l'ombre convergent vers la MÊME couleur : mathématiquement une compression
de contraste — le voile gris constaté. Avec lui, l'air éclairé s'allume et
l'air ombragé s'assombrit : le brouillard *structure* la lumière (le fog de
BotW) au lieu de l'aplatir. Tout le chantier tient dans cette inversion.

## 2. L'existant (inventaire, à réutiliser — pas à re-créer)

| Pièce | Fichier | Rôle actuel |
|---|---|---|
| Fog analytique | `sky.glsl` (`applyFog`, `skyGradient`) | extinction + ambient ciel ; PAS d'in-scatter solaire |
| Volumétrique | `landscape/shaders/volumetric.frag` | vrai raymarch : 20 pas ½-res, jitter IGN, 1 tap CSM + ombre de nuages par pas, phase — mais en **couche de correction** (shafts additifs plafonnés + assombrissement multiplicatif des rideaux), gaté aux ciels nuageux (couverture 0.30-0.40) |
| God rays | `godrays.frag` | radial screen-space, soleil à l'écran seulement |
| Lames de poussière | `lightshaft.frag` + `LightForm.shaft/sunLinked` (`data/forms/VisualForms.hpp`) | prismes additifs posables ; `sunLinked` suit DÉJÀ le soleil quantisé (direction, couleur, gate d'élévation, rebuild des lames aux pas du soleil — `LandscapeRenderer::drawLightShafts`), actif en mode intérieur |
| Mode intérieur | `game/FrameComposer.cpp` | soleil/ciel/fog coupés, `interiorAmbient` **constante** (LandscapeTuningForm) ; lumière clé + ombre perspective |
| GI | `gi.glsl` (`giAmbient`, `uGiCascade0`) | champ de radiance world-space, 8 slabs directionnels, fondu Classic au bord ; lumières RC-only |
| Météo + horloge | `WeatherForm` (crossfade ~30 s), soleil quantisé `shadowSunDirection` | l'heure et l'atmosphère existent déjà comme données vivantes |

Le point clé : `volumetric.frag` calcule déjà la visibilité par pas — il
corrige le fog au lieu d'en être la **source**. Le chantier inverse la
relation.

## 3. Les trois réponses

**Continuer le volumétrique ? Oui.** C'est le pilier « art direction +
soft GI feel + atmosphère » (CLAUDE.md §7) : dans un rendu stylisé sans
micro-détail, c'est l'air qui vend la profondeur et l'heure.

**Faut-il du raytracing ? Non, catégoriquement.** Le standard de
l'industrie pour le volumétrique est le **raymarch de la shadow map**,
généralement via une grille de **froxels** (frustum voxels) : Wrónski
(AC4, 2014), Hillaire (Frostbite, 2015), God of War, RDR2. Un tap CSM par
pas/froxel EST le test de visibilité — la CSM (2048, portée 800 m, arbres
castés par proxys métaballs) sert de structure d'accélération. Le RT
matériel n'apporte que pour des cas absents ici (occludeurs hors shadow
map, area lights), et **MoltenVK n'expose pas les extensions Vulkan de
raytracing** : sur la machine de dev M1, la question est close.

**Compatible Radiance Cascades ? Synergique.** Un voxel d'air a besoin
d'un terme ambient en tout point du monde — c'est exactement ce que
`uGiCascade0` fournit. Différence avec `giAmbient()` : l'air n'a pas de
normale, on moyenne les 8 slabs uniformément. Portée limitée au volume RC
autour de la caméra avec fondu vers Classic : le fog fait pareil (RC
près, `skyGradient` loin). Bonus : les lumières RC-only éclairent l'air
gratuitement, pénombres comprises.

## 4. Extérieur — briques V1→V4

### V1 — Le fog qui éclaire (analytique, ~gratuit) — ✅ FAIT (2026-07-23)

Le terme solaire est dans `applyFog` : la couleur du fog est
`skyGradient(viewDir) + uSunColor × pow(mu·0.5+0.5, uFogSunInfo.y) ×
uFogSunInfo.x` (phase HG repliée en lobe stylisé). Aucune marche, aucun
coût mesurable ; le fog face au soleil se dore, dos au soleil il
refroidit — le voile gris uniforme disparaît.

Câblage livré : `WeatherForm.fogSunScatter` (force, crossfadée par la
météo — Morning Mist 1.3 … Storm 0.05, `landscape.toml`),
`LandscapeTuningForm.fogSunPhase` (resserrement du lobe, global),
`AtmosphereParams` → `FrameComposer` (dans `base` : le reflet fogge
pareil) → `uFogSunInfo` (vec4 appendu au FrameUbo, règle append-only).
Sliders live « Fog sun scatter » / « Fog sun phase exp » au panneau
Fog & clouds. En intérieur, le mode coupe déjà la densité → terme sans
effet, rien à gater. La nuit, `uSunColor` s'éteint → extinction
automatique. Validation visuelle dev aux heures critiques : à faire.

### V2 — Le march devient la source

Refactor de `volumetric.frag` : au lieu de (shaft additif, multiplicateur
de rideau), la marche intègre réellement `(inscatter, transmittance)` par
pixel — `inscatter += T × densité × (sunColor × phase × visibilité +
ambient) × pas`, `T *= exp(-densité × pas)` — et le composite **remplace**
`applyFog` sur la portée marchée (1400 m) ; l'analytique V1 ne garde que
la queue au-delà. Le gate « ciels nuageux » devient un knob artistique.
Tout l'appareillage existant se réutilise : jitter IGN, tap CSM
`shaftShadow`, `cloudShadowFactor`, la densité par hauteur (mêmes
`uFogInfo`). Vérité à protéger : une seule source d'in-scatter — plus de
double comptage fog analytique + correction.

### V3 — L'ambient RC dans le march

Dans l'empreinte du volume RC, le terme ambient de V2 devient le sample
direction-moyenné de `uGiCascade0` (binding 11 à ajouter à la passe
volumétrique ; hors volume, retour au `skyGradient`, même fondu de bord
que `giAmbient`). Effets : fog verdi en clairière sous canopée, fog
sombre dans une vallée à l'ombre, air éclairé par les lumières RC-only.
La brique au meilleur ratio effet/effort une fois V2 posée.

### V4 — Froxels + reprojection temporelle (sur mesure F6 uniquement)

Le point d'arrivée AAA si le ½-res 2D montre ses limites (bruit, coût des
pas, lumières locales) : grille 3D (~128×72×64, tranches de profondeur
exponentielles jusqu'à ~400-500 m), deux passes compute (injection :
densité × lumières ; puis intégration avant→arrière), apply par pixel =
un fetch trilinéaire. Jitter + reprojection temporelle pour la stabilité.
Gains : lumières locales par froxel (les 16 du LightsUbo), densité
variable par volume (brume de fond de vallée, poussière d'intérieur),
coût indépendant de la complexité de la scène (~1-2 ms attendues M1).
Les caps compute/volumes existent (GL 4.6 + Vulkan). À n'engager
qu'après mesure : le budget vers la vsync 30 fps est étroit.

## 5. Intérieurs — le modèle « Helios », briques H1→H4

Référence : le mod Skyrim **Helios** (successeur de DIAL) — l'intérieur
reflète la météo extérieure et l'heure : ambiance claire par beau temps,
sombre + éclairs par orage, via l'ambiance et les lumières d'effet
(fenêtres, bounce, rayons). Chez Helios le critère est « intérieur à
fenêtres » ; ici il devient **géométrique** : sous une hauteur donnée,
l'espace est « enterré » et cesse d'être couplé au dehors — plus robuste
pour un moteur maison (une cave sous une maison à fenêtres se règle
toute seule).

Invariant : tout passe par les mécanismes existants — `WeatherForm` (la
météo est déjà crossfadée), l'horloge/soleil quantisé, `LightForm`
(`sunLinked` est le germe), le worldspace intérieur. Pas de « système
météo intérieur » parallèle.

### H1 — L'ambiance horaire

`interiorAmbient` cesse d'être une constante :
`ambiance = base(pièce) × daylight(heure) × facteurMétéo(atmos)`, où
`daylight` est la courbe jour/nuit déjà implicite dans le soleil et
`facteurMétéo` se dérive de l'atmos crossfadée (couverture, orage).
Données moddables §5 : champs appendus au worldspace intérieur (base,
poids du couplage) — le choix Form exact se tranche à la brique. L'orage
assombrit ; les éclairs restent du ressort des lumières (flicker existant).

### H2 — Raies héliotropes & lumières de fenêtre

Généraliser l'acquis `sunLinked` : l'intensité suit `daylight × météo`
(aujourd'hui : gate d'élévation seul) — une raie de fenêtre s'éteint la
nuit et pâlit sous les nuages ; couleur déjà héritée du soleil. Les
« window bounce » d'Helios = des `LightForm` ordinaires `sunLinked` SANS
`shaft` : sources posées près des fenêtres dont couleur/intensité suivent
le soleil. Zéro nouveau type — deux champs de comportement au plus.

### H3 — La règle « enterré »

Champ `buriedBelowY` sur le worldspace intérieur (défaut : désactivé).
Au-dessus du seuil : H1/H2 actifs. En dessous : ambiance et lumières
`sunLinked` reviennent à leurs valeurs statiques (le donjon ignore
l'heure). Bande de fondu de quelques mètres autour du seuil. Évaluation
**par position** : chaque lumière/raie selon SON Y, l'ambiance selon le Y
caméra — un escalier de cave traverse la frontière proprement.

### H4 — La poussière volumétrique réelle (= V4 en intérieur)

Quand V4 existe : densité de poussière par volume intérieur, raies de
fenêtre rendues par les froxels (les lames `lightshaft.frag` deviennent
un fallback low-spec ou disparaissent), la lumière clé et les locales
participent à l'air. Ne se planifie qu'après V4.

## 6. Ordre conseillé & mesure

`V1 → V2 → H1 → H2 → H3` — V3 dès que V2 est là (petite), `V4/H4`
seulement si la mesure F6 (les 4 spots) montre que le ½-res 2D ne suffit
pas ou que les lumières locales manquent à l'air. Chaque brique se valide
à l'œil aux heures critiques (aube, midi, crépuscule, nuit, orage) et au
F6 pour le coût — le poste `volumetric` existe déjà au panneau.
