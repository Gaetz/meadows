# WATER-RESEARCH — refonte du système d'écoulement de l'eau

> Étude des solutions existantes (2026-08-26), décidée après le constat
> dev : « les surfaces d'eau sont forcément plates/horizontales, on n'a
> pas de fleuves ou de rivières qui s'écoulent grâce au dénivelé ».
> La partie PEUPLEMENT du chantier terrain (B10-B13) est gelée jusqu'à
> la refonte. Contrainte cadre : tourner sur une machine relativement
> ancienne (référence dev : From Dust, consoles de 2011, 30 fps).

## 1. Le problème, posé proprement

Notre pipeline actuel produit des surfaces d'eau issues d'un
priority-flood : par construction, chaque bief est PLAT. Les briques
B9e/B9f ont imposé des gradients aux lits et aux surfaces de rubans —
mais le système reste un habillage sur une hydrologie de remplissage,
et l'eau ne « coule » visiblement que sur les ruisseaux. La question
n'est pas un tuning : c'est LE choix d'architecture que chaque jeu
open-world tranche. Trois écoles existent, plus des hybrides.

Fait d'industrie utile : la doctrine Valve (SIGGRAPH 2010, Vlachos,
« Water Flow in Portal 2 ») pose explicitement que « the water's
surface should never slope » — leurs rivières sont des BASSINS PLATS
étagés reliés par des chutes, l'écoulement étant une illusion de
shading (flow maps). À l'opposé, l'Unreal Water System (UE5) assume
des rubans de spline PENTUS avec un flow map dérivé des vitesses.
Aucun AAA open-world ne simule ses rivières en continu ; les
simulations réelles (From Dust, Cities: Skylines, Timberborn) sont des
jeux DONT l'eau est la mécanique centrale.

## 2. Taxonomie des solutions

### A. Bassins plats étagés + chutes (l'école Valve / Skyrim)

**Principe.** Le cours d'eau = une suite de biefs horizontaux (des
plans d'eau à niveaux décroissants), connectés par des CHUTES et des
RAPIDES (meshes/FX dédiés) aux ruptures de pente. L'écoulement de
surface est un flow map (un champ de vecteurs peint/calculé qui
distord les normales et fait défiler mousse et détails).

**Exemples.** Portal 2 / Left 4 Dead 2 (flow maps, doctrine « jamais
de pente »), Skyrim (plans d'eau par cellule + chutes), la plupart des
open-worlds pré-2015.

**Coût.** Quasi nul au runtime (shading seulement). Vieille machine :
prouvé depuis 2010.

**Fit Meadows.** TRÈS proche de ce qu'on a déjà : nos surfaces de
flood SONT des biefs plats — il manque la grammaire bief/chute :
segmenter chaque cours en bassins au bake (détection des ruptures de
pente), carve en marches, et générer chutes/rapides aux transitions.
Déterministe, streamable, moddable. Le reproche dev (« plat ») devient
une FEATURE si les biefs sont courts et les chutes lisibles — c'est
l'eau de montagne ; en plaine, les longs biefs plats sont physiquement
justes (un grand fleuve EST quasi plat : 0,02-0,1 m/km en réalité).

### B. Rubans pentus + illusion de flux (l'école UE5 / BotW)

**Principe.** La rivière = un mesh de spline dont la surface SUIT le
dénivelé (les points de spline portent des hauteurs) ; les vitesses
par point alimentent un flow map ; le terrain est carvé sous la
spline. Physiquement « faux » (une nappe d'eau ne tient pas sur une
pente) mais le standard industriel du lisible : l'œil accepte une
pente d'eau si le shading de courant est convaincant (mousse étirée,
vagues remontantes, écume aux obstacles).

**Exemples.** UE5 Water System (exactement ce modèle : « rivers allow
spline points to have varying heights », vitesse → flow map, carve du
terrain), BotW (rivières pentues + chutes en textures défilantes),
Waterways (Godot).

**Coût.** Shading un peu plus riche que A ; toujours trivial pour une
vieille machine.

**Fit Meadows.** C'est ce que B9e/B9f ont commencé : nos rubans
portent des surfaces à gradient réel. Ce qui manque est le RENDU du
courant (notre eau locale a un flow scalaire mais un shading de
surface quasi statique aux échelles moyennes) et la grammaire des
ruptures (rapides, chutes). Compatible avec A : B sur pentes douces,
A sur pentes fortes.

### C. Simulation heightfield temps réel (l'école From Dust)

**Principe.** L'eau = une couche de hauteur par cellule sur une grille
posée sur le terrain ; à chaque tick, des échanges entre cellules
voisines (équations shallow water, ou leur version « virtual pipes »,
plus rapide et stable — Dagenais et al. ; Kellomäki, « Large-Scale
Water Simulation in Games », a montré de grandes étendues à budget GPU
très limité, et que supprimer l'auto-advection ne change RIEN à la
perception). L'eau trouve son niveau, coule par le dénivelé, forme
chutes, crues et bassins ÉMERGENTS.

**Exemples.** From Dust (heightfield multi-couches sol/eau/lave sur
consoles 2011, moteur LyN, optimisation cache agressive — la carte
entière simulée, ~30 fps) ; Cities: Skylines (heightfield CPU) ;
Timberborn (cellulaire CPU par tuile, volume/pression/débit — et son
goulet est connu : mono-cœur, les chutes coûtent cher) ; les papiers
virtual pipes (temps réel prouvé, même en multi-couches).

**Coût.** Le vrai point : une fenêtre 512²-1024² à 2-4 m/cellule
autour du joueur tourne sur un GPU ancien (From Dust le faisait sur
X360). CPU possible mais mono-cœur douloureux (leçon Timberborn).

**Fit Meadows.** Le plus séduisant ET le plus structurant :
- fenêtre glissante autour du joueur, SOURCES alimentées par notre
  hydrologie bakée (têtes de cours + débits = aires de drainage) ;
- l'eau coule VRAIMENT : chutes émergentes, remous, crues possibles,
  interactions (barrer un ruisseau !) ;
- frictions réelles avec nos invariants : déterminisme (le sim est
  non-reproductible frame à frame → il doit rester PRÉSENTATION +
  requêtes gameplay locales, jamais sérialisé comme vérité),
  continuité aux bords de fenêtre (conditions aux limites = notre
  solution bakée), stabilité (CFL, pas de temps), autorité de
  l'auteur affaiblie (un monde moddé veut des rivières LÀ où la donnée
  les met).

### D. Simulation à l'équilibre, bakée (l'hybride « même physique, hors ligne »)

**Principe.** Faire tourner le MÊME solveur (virtual pipes) au bake,
par tuile, jusqu'à l'état stationnaire, avec les débits vrais du
réseau maître comme sources ; stocker le résultat : un champ de
PROFONDEUR d'eau + un champ de VITESSE par texel (deux masques de plus
dans la région). La surface d'eau qui en sort est physiquement
cohérente : pentue dans les rapides, plate dans les biefs, chutes aux
ruptures — sans un seul cycle de simulation au runtime.

**Exemples.** C'est la logique des flow maps bakés poussée au bout ;
les outils de terrain (Houdini et consorts) font exactement ça pour
les jeux qui veulent « l'eau juste » sans payer la sim.

**Coût.** Temps de bake (quelques secondes/tuile pour converger) +
stockage (2 champs). Runtime : rendu seulement.

**Fit Meadows.** Le meilleur alignement avec NOS invariants :
déterministe bit-exact (c'est un bake), streamé comme le reste,
moddable (les sources dérivent des Forms/du seed), et le rendu peut
consommer profondeur+vitesse par texel (surface d'eau en heightfield
local au lieu de rubans — finis les artefacts de jonction de rubans).
Limite : statique (pas de crue dynamique, pas de barrage joueur).

### E. Compléments ponctuels (pas la colonne vertébrale)

SPH/FLIP (particules) : hors budget en étendue, réservé aux moments
scriptés. Wave Particles (Yuksel) : sillages/impacts, complément
élégant plus tard. FFT (océan) : houle du large, orthogonal aux
rivières.

## 3. Comparatif

| Critère | A. Biefs+chutes | B. Rubans pentus | C. Sim temps réel | D. Sim bakée |
|---|---|---|---|---|
| Écoulement lisible | Bon (chutes) | Bon (flow fort) | Excellent (vrai) | Excellent (vrai, figé) |
| Vieille machine | Trivial | Trivial | OK (prouvé 2011) mais LE poste de coût | Trivial au runtime |
| Déterminisme/saves | Parfait | Parfait | À encadrer (présentation seule) | Parfait |
| Monde infini/streaming | Natif | Natif | Fenêtre + bords à gérer | Natif |
| Contrôle auteur/mods | Fort | Fort | Faible | Fort |
| Interactions (barrages, crues) | Non | Non | OUI (l'argument) | Non |
| Effort d'implémentation | Moyen | Faible-moyen | Élevé | Moyen-élevé |
| Risque | Faible | Faible | Moyen-élevé | Moyen |

## 4. Recommandations (proposées à l'arbitrage dev)

1. **La colonne vertébrale : D** — le solveur virtual-pipes au bake,
   sources = débits vrais du réseau maître, sortie = profondeur +
   vitesse par texel. C'est la seule option qui donne « l'eau
   physiquement juste » SANS toucher aux invariants (déterminisme,
   streaming, mods) ni au budget vieille machine. Elle remplace le
   couple flood-plat + rubans par UNE vérité d'eau en champ.
2. **Le rendu : la grammaire A+B par-dessus D** — surface d'eau en
   heightfield local (depuis le champ de profondeur), flow maps depuis
   le champ de vitesse, chutes/rapides générés aux ruptures détectées
   dans le champ. Les biefs plats deviennent un CHOIX local du
   solveur, plus une fatalité du flood.
3. **L'option ambition, en couche : C** — une fenêtre de sim temps
   réel autour du joueur, INITIALISÉE par l'état D (qui fournit aussi
   les conditions aux bords). D d'abord la rend possible proprement ;
   sans D, C n'a ni bords ni état de repos. À décider après D, selon
   l'envie de gameplay (barrages, crues, sabotage de moulins…).
4. **E plus tard** (sillages, océan FFT) — orthogonal.

Prochaine étape proposée si D est retenu : prototype du solveur sur
UNE tuile (convergence, coût de bake, les deux champs), et un rendu
heightfield de sa sortie sur la zone du fleuve du spawn.

## Sources

- [Water Flow in Portal 2 (Vlachos, SIGGRAPH 2010)](https://www.slideshare.net/slideshow/siggraph-2010-water-flow-in-portal-2/4899875)
- [Valve Developer Community — Water (shader)](https://developer.valvesoftware.com/wiki/Water_(shader))
- [Graphics Runner — Animating Water Using Flow Maps](http://graphicsrunner.blogspot.com/2010/08/water-using-flow-maps.html)
- [UE5 — Water Body Actors](https://dev.epicgames.com/documentation/en-us/unreal-engine/water-body-actors-in-unreal-engine) / [Water System](https://dev.epicgames.com/documentation/en-us/unreal-engine/water-system-in-unreal-engine)
- [From Dust (Wikipedia — LyN engine, simulation par règles)](https://en.wikipedia.org/wiki/From_Dust)
- [Kellomäki — Large-Scale Water Simulation in Games (thèse)](https://researchportal.tuni.fi/en/publications/large-scale-water-simulation-in-games)
- [Kellomäki — Fast Water Simulation Methods for Games (survey)](https://dl.acm.org/doi/abs/10.1145/2700533)
- [Kellomäki — Rigid Body Interaction for Large-Scale Real-Time Water Simulation](https://onlinelibrary.wiley.com/doi/abs/10.1155/2014/580154)
- [Dagenais et al. — Real-Time Virtual Pipes Simulation for Small-Scale Shallow Water](https://www.semanticscholar.org/paper/c116bf3e70d05d840a62300cd3516ba6dd44edda) / [Extended virtual pipes (CAG 2018)](https://www.sciencedirect.com/science/article/abs/pii/S0097849318301341)
- [Deep Dive: Timberborn's water mechanics (Game Developer)](https://www.gamedeveloper.com/design/deep-dive-timberborn-s-water-mechanics)
- [Timberborn — perf et goulets de la sim d'eau](https://pixelnitro.com/timberborn-low-fps-fix-2026-guide-to-optimizing-water-waterfalls-and-high-pop-colonies/)
