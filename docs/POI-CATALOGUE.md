# Catalogue des points d'intérêt naturels et des paysages

> Référence du chantier « plan de POI » (`docs/PAYSAGE.md` §7.6). Le
> générateur place d'abord les points d'intérêt (POI), puis sculpte le
> terrain autour (la règle du triangle de Breath of the Wild, les lieux
> placés de Skyrim). Ce catalogue est **la table de types** que le plan
> tire, conditionnée par le biome et les conditions de site, et **la table
> des paysages** (caractères) que l'on traverse entre deux POI. Il
> s'enrichit à mesure que les noyaux existent dans `PoiPlan.cpp` ; une
> ligne sans noyau est une intention, pas une promesse.
>
> Base : la taxonomie géomorphologique (formes fluviales, glaciaires,
> éoliennes, karstiques, côtières, volcaniques) et les catégories de sites
> naturels touristiques (cascades, canyons, arches, geysers et sources
> chaudes, grottes, falaises, sommets, plages, îles, forêts).

**Légende.** Étage : **G** grand (1 par carte de 8 km, 400-600 m, le but
lointain), **M** moyen (16 par carte, 150-250 m, le but à 3-5 min), **P**
petit (~300 par carte, 10-40 m, le détail à 50 m), **M (région)** = un
caractère de paysage (§E), pas un site. Biomes (palette `paletteIdFor`) :
0 tempéré, 1 aride, 2 alpin, 3 toundra, 4 subalpin, 5 steppe. Conditions
lues sur `worldSampleAt(site)` : côte, massif, étage, rivière à < 300 m
(réseau maître), bassin fermé, dureté. Noyau = la forme stampée par le plan
(levée de base, bassin, gradin, pad). Approche = comment le chemin l'aborde.

## A. Relief et roche

| POI | Étage | Conditions | Noyau | Approche |
|---|---|---|---|---|
| Sommet pyramidal (pic) | G/M | massif > 0,5 ou étage ≥ 2 ; tous biomes | cône 25-35°, 150-600 m | de dessous, le col mène au pied, un flanc grimpable |
| Aiguille / dent rocheuse | M | massif, alpin/subalpin | cône raide (40°) étroit, 120-250 m, sommet dur | on la contourne, elle se voit de partout |
| Crête-échine à cols | M | collines, massif | échine 1-2 km à 2-3 selles | on la franchit par un col, vue des deux côtés |
| Mesa / plateau-témoin | M | plaine, plateau ; aride/steppe privilégié | plateau plat, rebord raide 40-120 m | rampe unique ou éboulis ; en haut = belvédère |
| Butte / dôme | M/P | plaine, tempéré/steppe | dôme 60-200 m | directe, de tous côtés |
| Gradin / escarpement | M | changement d'étage (bord de plateau) | demi-plan lissé 40-120 m sur 1-3 km | longé, puis une brèche |
| Falaise de gorge / canyon | M/G | plateau + lit de rivière ; aride, tempéré | échine négative 60-200 m, fond plat | par le fond (rivière) ou par le rebord |
| Col / brèche | M | entre deux reliefs | selle | c'est le chemin lui-même |
| Arche naturelle | P/M | aride, côte ou canyon (grès) | socle stampé ; l'arche (deux piliers + linteau) est un mesh posé plus tard | on passe dessous |
| Cheminées de fée / hoodoos | P (groupe M) | aride, badlands | champ de cônes 10-30 m sur 300-600 m | on traverse le champ |
| Piliers / tours karstiques | M | tempéré humide, calcaire (dureté > 0,7) | tours 50-150 m sur 500-800 m | on serpente entre |
| Chaos de blocs / champ d'erratiques | P | alpin, toundra, tempéré de montagne | semis de rochers (scatter dirigé) sur une butte | on y grimpe |
| Rocher en équilibre / menhir naturel | P | partout | un rocher (mesh) sur une butte de 10 m | détail de route |
| Éboulis / pierrier | P | pied d'aiguille, massif | pente 30° sans végétation (dureté, masque scree) | on le longe |
| Doline / gouffre / entrée de grotte | P/M | karst tempéré, plateau | dépression 20-60 m, parois raides, entrée (mesh) | on descend ; future entrée de donjon |
| Cône volcanique / cratère | G/M | aride, subalpin ; rare | cône tronqué 150-400 m + cratère (bassin au sommet) | on monte au bord ; lac de cratère possible |
| Champ de lave / malpaís | M (région) | près d'un cône | plateau rugueux noir (dureté 1, palette aride) | on le contourne |
| Badlands | M (région) | aride, steppe, argiles | roulis serré ±30 m / 150 m, très disséqué (budget rough) | labyrinthe, vue depuis une butte |
| Cirque glaciaire | M | alpin | bassin en fer à cheval à parois de 200 m, lac de cirque au fond | on entre par le verrou |
| Vallée en auge / verrou | M | alpin → subalpin | fond plat 300-600 m, parois raides, verrou rocheux | on la remonte |
| Moraine / drumlins | P | subalpin, toundra | bourrelets 10-30 m allongés | détail de marche |
| Névé / glacier suspendu / champ de neige | G (accent) | alpin > 950 m | plateau neigeux (ligne de neige locale abaissée) | vue, pas d'accès |
| Dunes | M (région) | aride, côte sableuse | roulis ±20 m / 120 m orienté (éolien) | on les gravit |
| Yardangs | P | aride | crêtes allongées 5-15 m alignées | couloirs |
| Plaine salée / playa | M (région) | aride, bassin fermé | plat absolu, palette claire, pas d'eau | on la traverse (horizon) |

## B. Eau

| POI | Étage | Conditions | Noyau | Approche |
|---|---|---|---|---|
| Cascade | M | gradin + rivière (débit > seuil) | marche 20-80 m sur le lit, vasque au pied | d'en bas (bruit, brume), puis par le rebord |
| Série de cascades / escalier | M | plateau + rivière | 3-5 marches de 5-15 m | on les remonte |
| Rapides / gorge d'eau vive | P | pente du lit > 3 % | lit rétréci, rochers | gué impossible ; pont plus tard |
| Lac de plaine | M | bassin fermé en plaine (POI « en creux ») | bassin plat 300-900 m, 5-15 m de profondeur | par la rive ; le rebord = belvédère |
| Lac de cirque / tarn | M | alpin | bassin rond 100-300 m dans un cirque | par le verrou |
| Lac de cratère | M | cône volcanique | bassin au sommet | par le bord du cratère |
| Marais / tourbière | M (région) | plaine humide, tempéré/toundra | roulis ×0,4, humidité +0,6, mares (petits) | on le contourne ; passerelle future |
| Méandre / bras mort | P | fleuve en plaine | lit sinueux, bras mort = mare | détail de rive |
| Confluence | M (site) | deux rivières | plat alluvial (pad) | site de village |
| Source / résurgence | P | pied de gradin, karst | mare + ruisseau naissant | détail de route ; hameau proche |
| Source chaude / geyser / mares de boue | P/M | volcanique, rare | mares colorées, vapeur (fx) | curiosité |
| Delta / estuaire | M (site) | embouchure | plat, chenaux, vasières | site de bourg ou de port |
| Île lacustre | P | lac > 500 m | butte dans le bassin | vue ; accès futur |
| Oasis | M | aride + source | bosquet (palette 0 forcée) autour d'une mare | refuge ; site de hameau |

## C. Côte

| POI | Étage | Conditions | Noyau | Approche |
|---|---|---|---|---|
| Promontoire / cap | M | côte, étage ≥ 1 | échine qui s'avance en mer, 60-150 m | belvédère de bout du monde |
| Falaise de mer | M (ligne) | côte dure (dureté > 0,6) | gradin 40-120 m au rivage | longée par le haut |
| Aiguille / stack de mer | P/M | devant une falaise | cône dans l'eau | vue |
| Arche de mer | P | falaise | arche sur le rivage | vue ; passage à marée basse (fx futur) |
| Crique / anse | M | côte, bassin ouvert sur la mer | bassin en demi-cercle, plage | accès par un sentier descendant |
| Plage et cordon dunaire | M (région) | côte molle | plat sableux, dunes derrière | marche libre |
| Lagune / marais salant | M (région) | côte molle, plaine | bassin saumâtre peu profond, plat | on contourne |
| Fjord / ria | G | côte + massif, alpin | bras de mer dans une auge | vue depuis le haut |
| Îlot / archipel | M | mer peu profonde | buttes dans la mer (bordure « Mer » existante) | vue ; bateau futur |
| Tombolo / flèche | P | côte molle | cordon de sable | détail |

## D. Végétation et sol

Des formes « douces », souvent le caractère traversé entre deux POI.

| POI | Étage | Conditions | Noyau | Approche |
|---|---|---|---|---|
| Arbre solitaire remarquable | P/M | prairie, steppe | butte de 10 m + arbre géant (scatter dirigé) | amer de plaine |
| Bosquet ancien / clairière | P/M | tempéré | creux ou replat + densité forêt ×2 / clairière ×0 | halte |
| Forêt de géants | M (région) | tempéré humide, subalpin | densité ×1,5, échelle des arbres ×1,8 | on y entre |
| Prairie de fleurs / alpage | M (région) | subalpin, tempéré | roulis ×0,8, herbe haute colorée | halte, vue |
| Lande / bruyère | M (région) | tempéré pauvre, subalpin | roulis ×1, palette 4/5, arbres rares | marche ouverte |
| Forêt brûlée / pétrifiée | M (région) | aride, volcanique | arbres morts, sol noir ou gris | ambiance |
| Tourbière boisée | M (région) | toundra, tempéré froid | humide, arbres rabougris | contournée |
| Champ de rochers moussus | P | tempéré humide | blocs + mousse | détail |

## E. Les paysages (caractères) par biome

Ce que l'on traverse entre deux POI. Un caractère = un multiplicateur
d'amplitude et de longueur d'onde du roulis, un biais de palette, d'humidité
(lits, mares) et de dureté, et des règles de végétation lues plus tard par
le scatter.

| Biome | Caractères (relief × λ, palette, eau, végétation) |
|---|---|
| 0 tempéré | prairie roulante (×1, ×1) · bocage à haies (×0,8, bosquets alignés) · forêt de feuillus dense (×1,2, ×0,8) · vallée fluviale à prés (×0,5, humide) · marais / tourbière (×0,4, humide +0,6) · plateau rocailleux à mesas (×0,7, ×1,4, dur) · gorges calcaires (budget rough, dur) · lande (×1, ×1,6, palette 4) |
| 5 steppe | steppe herbeuse ouverte (×0,6, ×2) · collines pelées (×1, ×1) · badlands (×1,5, ×0,3, rough) · oued et terrasses (×0,5, lit sec) · bosquets-galeries le long de l'eau |
| 1 aride | plateau de mesas (×0,7, ×1,4) · champ de dunes (éolien) · playa / salar (×0) · champ de hoodoos · canyon de strates colorées · champ de lave · oasis |
| 4 subalpin | alpage fleuri (×0,8) · forêt de conifères (×1,2, dense) · lande à rhododendrons (×1) · tourbière d'altitude (×0,4, humide) · pierriers et chaos (×1, dur) · vallée en auge (×0,5 au fond, parois) |
| 2 alpin | arêtes et aiguilles (×2, dur, rough) · cirques et tarns · névés (neige) · éboulis (×1, scree) · plateau karstique à lapiaz (×0,8, dur) |
| 3 toundra | toundra à polygones (×0,3, ×2) · lacs de fonte en chapelet (bassins P) · moraines et erratiques · forêt-toundra rabougrie · falaises de côte froide (fjord) |

## F. Règles de répartition

Le plan (`PoiPlan.cpp`) lit ces règles ; les tests les vérifient.

1. **Un POI est tiré par son étage de site.** Le type vient de la table du
   biome × (côte, massif, étage, rivière à < 300 m, bassin fermé, dureté).
   Un site côtier tire dans C, un site traversé par une rivière dans B, le
   reste dans A puis D ; les lignes « M (région) » sont des caractères (§E),
   jamais des sites.
2. **Un grand par carte** : pic, cône volcanique, fjord, grande mesa, cirque
   majeur. Jamais deux grands du même type sur deux cartes voisines (le hash
   de cellule est confronté aux 8 voisines).
3. **Jamais deux moyens du même type adjacents dans le graphe** (re-tirage sur
   le hash quand le voisin de rang inférieur a le même type) : la variété est
   une contrainte du graphe, pas une moyenne.
4. **Le caractère d'une arête** est celui de la région du POI le plus proche,
   avec la règle de contraste : le long d'une marche A → B → C le caractère
   change au moins une fois (A et C ne partagent pas le leur). Les caractères
   humides (marais, vallée, tourbière) ne sont tirés qu'avec de l'eau à
   < 500 m ; les caractères durs (gorges, mesas, lave) qu'avec dureté > 0,6
   ou étage ≥ 2.
5. **L'eau fait le lien** : une rivière qui relie deux POI est un chemin
   (corridor de rive) ; cascade et gorge se posent là où une rivière croise un
   gradin — le plan lit le réseau maître avant de typer (B et A sont couplés).
6. **Les petits suivent les moyens** (la gravité de BotW) : des satellites
   typés par le moyen (pierrier sous l'aiguille, mares autour du marais,
   erratiques sur la moraine, stacks devant la falaise).
7. **Sites réservés** pour le peuplement futur : confluence, delta / estuaire,
   crique abritée, pied de cascade, rebord de mesa, oasis, col majeur →
   `MarkerForm.kind = "site:<type>"` ; les POI visibles → `"poi:<type>"`.
8. **Ce qui n'est pas du terrain** (arche, menhir, arbre géant, geyser) est un
   socle stampé plus un marqueur : la forme finale (mesh, fx) vient avec le
   peuplement ; le plan ne promet que la place et la visibilité.

## G. La règle du triangle, en une table

| Principe (Nintendo, GDC 2017) | Mécanisme | Mesure (`poi visibility diagnostic`) |
|---|---|---|
| Le POI est un triangle : silhouette lisible, grimpable | profils coniques (25-35°), la mesa à flancs rectilignes | pente des flancs dans [25°, 35°] |
| Trois tailles | grand / moyen / petit (voir légende) | densités par carte |
| Cacher puis révéler le long d'une route | un ou deux écrans transversaux (40-90 m) à 40-60 % de chaque arête, avec une encoche sur le corridor | LOS départ → destination vraie, pied d'écran fausse, crête vraie, sur ≥ 80 % des arêtes |
| Le choix : contourner ou grimper | un écran n'est jamais un mur : col de pente ≤ 10 %, flancs ≤ 30°, extrémités libres | test par écran |
| La gravité | les petits se concentrent près des moyens (80 % à < 500 m, 40 % ailleurs) | densité des petits selon la distance au moyen |
| Le sommet récompense | depuis chaque sommet moyen, ≥ 2 autres moyens et le grand visibles | ≥ 90 % des sommets |
