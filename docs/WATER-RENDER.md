# WATER-RENDER — le rendu de l'eau simulée (option C)

> Référence du rendu de l'eau temps réel. À lire avant de toucher
> `engine/terrain/WaterSim.*`, `engine/render/landscape/WaterSystem.*`
> ou `water_surface.glsl`. Ce document est aussi le fichier de reprise
> du chantier : il fige l'architecture ET les leçons mesurées — ne pas
> les repayer.

## 1. Architecture (décidée 2026-08-27, après l'accumulation)

La simulation (WaterSim, virtual pipes fenêtré 512 m @ 2 m, 30 Hz sur
worker — voir le plan du chantier C) est LA vérité de l'eau proche.
Trois principes de rendu, nés de l'échec de l'empilement précédent
(nappe déplacée + plongées sèches + jupes séparées = trois géométries,
chacune ses seuils, clignotements et orphelines) :

1. **UNE seule géométrie : un maillage fermé par snapshot.** Le worker
   qui extrait le snapshot construit aussi le maillage : faces de
   dessus des cellules mouillées (nœuds partagés, hauteur = moyenne
   des surfaces des cellules mouillées adjacentes) + faces latérales
   aux frontières mouillé/sec DANS le même maillage (étanche par
   construction). Le main thread ne fait qu'uploader les buffers.
   Pas de grille déplacée par texture, pas de jupes séparées, pas de
   nœuds secs « plongés sous la berge ».
2. **L'étendue de l'eau est définie par la géométrie, jamais par un
   discard.** Le fragment shader (WATER_SIM dans water_surface.glsl)
   ne fait QUE du shading : corps par colonne simulée, advection par
   le courant, murs par la normale géométrique, ménisque, fondu de
   bord de fenêtre. Les discards de sécheresse par seuil bilinéaire
   ont fait disparaître des surfaces entières (mesuré) — interdits.
3. **Le mouillé/sec est hystérétique et persistant** (WaterSimState::
   wetMask) : une cellule devient mouillée au-dessus d'un seuil haut,
   ne redevient sèche que sous un seuil bas (films rapides : seuils
   réduits). Sans hystérésis, les films au voisinage du seuil
   clignotaient par tick et chaque disparition faisait apparaître des
   murs orphelins chez les voisines.

Rôles autour :
- **Le baké est DONNÉE, jamais géométrie, dans le rect de confiance** :
  lacs = réservoirs épinglés (niveau tenu, sortie bornée au débit de
  déversoir), rivières entrantes = sources (loi de largeur inversée),
  réseau maître = aires vraies. Il ne se REND qu'au-delà du rect ;
  la bande de fondu lie les deux mondes. (Deux réseaux affichés en
  même temps = rubans « montant les pentes » à côté de l'eau sim.)
- **La mer** reste la nappe analytique ; les cellules mer de la sim
  sont épinglées et publiées SÈCHES.
- **Volumes debug** (panneau Water, mode « Volumes debug ») : une
  colonne translucide par cellule mouillée — la vérité brute de la
  sim, indépendante de tout le shading. Premier réflexe de diagnostic,
  avec le Seam overlay (nappe sans depth-test) et le dump
  (`cooker water-replay`).

## 2. Le maillage (construit dans extractSnapshot, worker)

- Grille de nœuds (n+1)² aux coins de cellules ; un nœud porte la
  MOYENNE des surfaces des cellules mouillées qui le touchent.
- Dessus : 2 triangles par cellule mouillée (connexité-8 exigée — un
  filet descend une paroi en DIAGONALE ; le test à 4 voisins a effacé
  des chutes entières).
- Faces latérales : à chaque frontière mouillé/sec, un quad des deux
  nœuds de dessus vers bottom = max(terrain de bord moyen,
  surface − colonne) − petite marge. **La face ne dépasse JAMAIS la
  colonne qui la soutient** : descendre au sol du voisin sec plantait
  des murs de 3-10 m sous chaque filet de 10 cm traversant une pente
  (mesuré — « surfaces qui montent sur la montagne »). Un vrai front
  profond (barrage, lèvre de chute) garde son grand mur.
- Vertex = pos3 + uv2 (uv = position dans les textures sim, pour le
  shading par fragment).

## 3. Leçons mesurées (le grand livre — ne pas repayer)

Simulation :
- Cap de vidange 25 %/substep (100 % = paquets de m³ sautant une
  cellule/substep sur les parois — damier).
- Réservoirs épinglés : niveau TENU (recharge instantanée) + SORTIE
  bornée (reservoirOutflow ~2 m³/s/cellule). Recharge bornée = le lac
  s'effondre en cône ; sortie non bornée = ~6 200 m³/s mesurés (la
  gorge se remplit, le front escalade les pentes opposées).
- Épinglage : INTÉRIEUR du masque seulement (érodé d'un texel 8 m — le
  masque rasterisé à 2 m déborde sur les berges = sources
  artésiennes) ; jamais les étangs sans masque (bbox).
- Films rapides : le seuil de séchage 2 cm gomme les chutes (le film
  d'une paroi court en cm) — publier dès ~4 mm si vitesse > 1,5 m/s.
- CFL : c = √(g·texel) → dt ∝ √texel ; 1/30 s stable à 2 m.
- Sol de sim = render::terrain::height COMPLET (base + détail +
  patches) — toute autre source ré-ouvre la classe de bugs « sim vs
  rendu » qui a tué l'option D.
- **L'eau DORMANTE vient du BAKÉ, jamais de l'init** : le warm start
  priority-flood remplissait toute cuvette fermée par la fenêtre
  jusqu'à son col (lac fantôme de 138 m / 9,5M m³ mesuré au replay,
  ruisselant sur les versants). initWindow démarre SEC (mer seule),
  le pre-roll sans warm start ; lacs = pins, rivières = sources — la
  sim déplace l'eau, elle n'en invente pas.
- **Les bandes de scroll entrent SÈCHES** (même doctrine) : l'init
  « flood borné par le bord survivant » peignait le niveau d'un lac
  épinglé sur toute bande entrante (3,9M m³ mesurés au replay en
  volant près du lac). Lacs re-rasterisés par pinLakes après chaque
  scroll, rivières par les sources ; le wetMask se décale avec les
  plans (sinon hystérésis désalignée → flicker).
- La sim attend la tuile bakée sous la caméra (sinon elle résout sur
  le terrain analytique de repli, faux de plusieurs mètres).

Shading :
- Un NaN d'un seul fragment empoisonne la chaîne de bloom par TUILES
  (rectangles noirs) : garde sur normalize(cross(dérivées)) + sanitize
  final. Jamais de normalize sans test de longueur dans l'eau.
- L'underside (vue de dessous) doit être coupée sur les facettes
  murales sous l'œil (couture horizontale à hauteur de caméra sinon).
- Le lait de torrent et le blanc des murs exigent la VITESSE (un
  glissement lent lisait comme une bâche blanche).
- Fresnel sim ×0,6 : sans miroir planaire (ciel analytique seul), le
  plein fresnel peint les rivières en blanc-ciel au rasant.
- Le feather d'épaisseur optique dissolvait le corps des ruisseaux
  drapés — la colonne SIM compte dans le fondu.
- **La COULEUR ne lit jamais le champ simulé** : toute variante
  (brute, lissée croix, lissée 3×3) peignait des taches évolutives —
  ronds/croix/losanges — au rythme des fluctuations du champ, même à
  écoulement stable. La couleur sim = absorption par épaisseur
  OPTIQUE (géométrie, stable) + teinte constante ; le champ ne sert
  au visuel que pour la direction d'advection (lisse par nature).
- **La peau d'un lac dans le rect appartient à la SIM ; la frontière
  se tue par IDENTITÉ + passation nette, pas par retrait** : la
  variante « lacs publiés secs, nappe bakée partout » (règle mer
  étendue) a été essayée et ANNULÉE — la nappe bakée rasterisée
  SURPLOMBE la lèvre de sortie : une surface flottant au-dessus du
  vide à chaque exutoire, l'eau passant à côté (mesuré dev ; et sa
  règle de cull décapitait les cascades de lac au passage). Le
  maillage sim épouse l'eau réelle — c'est lui le lac dans le rect.
  Le liseré verdâtre au bord de fenêtre se corrige au SHADER :
  (a) parité TOTALE des recettes, aplat LOD inclus (le sauter =
  contraste de texture pleines-rides contre aplati à 200 m+) ; les
  cellules sim calmes exécutent le chemin partagé tel quel, seules
  les cellules en écoulement gardent le champ immobile (l'advection
  deux-phases sur le flux sim bruité faisait les anneaux) ;
  (b) passation par type au bord : rubans = bande de fondu (leurs
  cours divergent du baké), lacs = coupe GÉOMÉTRIQUE exacte
  (sim rend edgeM > fadeStart, nappe bakée rend edgeM <= fadeStart,
  coplanaires, même recette → coupe invisible ; l'epsilon de
  smoothstep laissait un anneau sec de ~40 cm) ; l'eau calme reste
  PLEINE jusqu'à la coupe — la fondre vers le sol réfracté peignait
  la bande verte translucide autour de la fenêtre.
- **La sobriété doit être SYMÉTRIQUE — la recette sim = la recette
  mer, à l'identique** : chaque simplification appliquée au seul
  chemin sim (normale plate, fresnel plafonné 0.28, ciel sans soleil)
  a redessiné le rect de confiance par CONTRASTE avec le baké/mer
  autour (mesuré dev sur les grands lacs). L'eau calme a UNE recette :
  waveNormal(xz, t) de la mer telle quelle (sans échelle de ride, sans
  advection deux-phases — c'est l'ADVECTION qui faisait respirer les
  anneaux concentriques au cycle de 3 s, pas les vagues), fresnel
  ×0.75 commun, skyWithSun. Le « brûlage au disque solaire » attribué
  au ciel était en fait le handle de texture non lié (leçon backend
  ci-dessous) — la formule commune est sûre. Simplifier l'eau sim
  seule est interdit : on simplifie TOUT le rendu d'eau ou rien.
- L'underside est une décision UNIFORME (caméra immergée via
  uSubmersionInfo.x) — le test par fragment coupait toute eau en
  pente d'une ligne nette à hauteur d'œil (bug d'avant la sim).
- Murs : seuil de pente serré (~81°+) — à 45-60° une nappe drapée est
  une surface, pas un mur (collines rayées sinon).

Intégration :
- Un seul job sim en vol (Phase-5) ; sources maître calculées sur le
  worker ; textures détruites/recréées par tick (chemin info-map
  éprouvé) ; FrameUbo append-only + rebuild propre.
- La vitesse caméra n'invalide JAMAIS la fenêtre : elle SCROLLE (le
  scroll préserve l'intérieur bit-exact) — l'invalidation à 25 m/s
  faisait « rejouer la cascade » à chaque déplacement en spectateur
  (mesuré dev). Seul un téléport > demi-fenêtre ré-initialise, et le
  pre-roll épingle les lacs PUIS fait une rafale (~15 s de sim) pour
  arriver « déjà en train de couler ».
- **Backend d'abord quand l'artefact ERRE** : des symptômes qui
  changent de forme à chaque retouche shader (ronds → croix → brûlure
  blanche → cyan) = données d'entrée indéfinies, pas une erreur de
  shading. Racine réelle : la whitelist d'upload GL46 refusait
  R32F/RGBA16F avec pixels → createTexture échouait → handle 0 → le
  shader lisait de la mémoire indéfinie. Vérifier le HANDLE avant de
  soupçonner le shading ; le replay prouve l'état sim sain en une
  commande.
- `cooker water-replay <dump> <prefix> [substeps]` : TOUT correctif de
  simulation se vérifie sur un dump AVANT de re-déranger le dev.

## 4. Restes connus / prochaines briques

- C4 : nage/courants/submersion lisant le snapshot dans le rect.
- C5 : écume de cascade dédiée, foam de rive des grands lacs, retraite
  de la WaterInfoMap (les rubans ne se rendent plus qu'au loin), purge
  éventuelle des champs TRG3 de l'option D (le solveur reste : pre-roll
  + oracle).
- Régénération de l'eau COURANTE au retour : **TRAITÉE (2026-08-28)**
  par les deux leviers prévus. (a) Cache LRU de session des états de
  fenêtre (WaterSystem::simCache, cap 4 × ~2-3 Mo, jamais sérialisé —
  doctrine save §2.4 intacte) : un téléport pousse l'état évincé, le
  retour le REPREND via `chooseCachedWindow` (helper pur doctesté,
  seuil 25 % de recouvrement) + scrollWindow — pas de re-solve.
  Invalidé au sculpt, au changement de bodies, au reset. Le cache
  couvre aussi le RETOUR EN MARCHANT (le scroll n'évince jamais) via
  les MIETTES DE PAIN : une copie de l'état rejoint le cache toutes
  les demi-fenêtres de trajet, et `scrollWindow` remplit ses bandes
  ENTRANTES depuis la miette la plus récente qui les couvre (eau
  copiée — profondeur, tuyaux, mémoire de mouillage — jamais
  inventée : la doctrine bandes-sèches interdit l'invention, pas le
  souvenir). (b)
  Révélation « settle-gated » (SimConfig::settleGated, défaut ON,
  case au panneau Water) : après pre-roll ou reprise, la fenêtre
  simule DERRIÈRE l'affichage baké (simMapInfo publie w=0, maillage
  non dessiné) jusqu'au calme — |Δvolume publié| relatif < 2e-3
  pendant 8 résultats consécutifs, plafonné à 3 s. Statut
  « settling... » sur la ligne d'état.

### Décisions du 2026-08-28 — le modèle D ne revient pas

- **Re-bake de l'eau D écarté** (question dev, inventaire complet à
  l'appui) : le solveur `solveSteadyWater` garde ses DEUX rôles
  sanctuarisés — pre-roll de la fenêtre (~2 s worker) et oracle des
  tests — et `TileBakeParams::solveWater` reste OFF. Le re-baker par
  tuile coûtait +13 s/tuile (mesuré à l'époque du branchement) pour un
  gain nul : l'échec RENDU de D était structurel (grille de solve 8 m
  incapable de suivre le terrain 2 m — 3 validations échouées), et le
  besoin réel derrière la question (l'eau « déjà en place ») est
  couvert par le cache LRU + settle-gate ci-dessus, runtime purs.
- **Carve du terrain depuis une lame d'eau résolue écarté** : le
  terrain est DÉJÀ adapté à l'eau par la topologie de drainage —
  fleuves IMPRIMÉS avant l'érosion (chenal + plaine + lit monotone),
  channel keep du fastscape, lits creusés par tier avec gués, bassins
  de lacs carvés par distance à la rive, érosion fine pilotée par le
  champ de débit partagé entre tuiles. Carver depuis une lame 8 m
  graverait la classe de bugs 8 m/2 m dans le terrain lui-même.
  Backlog retenu (sans solveur, données de masques existantes, bump
  kTileBakeVersion à prévoir) : plages/berges lacustres, visibilité
  des petits ruisseaux.
- updateTexture RHI (destroy+create par tick = provisoire éprouvé).
- Cascade FX (voile + écume projetée) par-dessus la goulotte.
