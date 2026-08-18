# Mémoire du monde infini — empreinte et fragmentation

> Plan de raisonnement (2026-08-18, question dev) : « plus on va loin et
> plus on modifie de cellules, plus la mémoire se remplit. Décharge-t-on ?
> Sauvegarde-t-on l'état pour le recharger au retour ? Quid des PNJ, des
> modifications de terrain, des arbres coupés ? » Périmètre : empreinte
> ET fragmentation. Les décisions prises ici doivent respecter §5 (une
> save = une couche de patches) et le contrat Sandbox (génération = 
> fonction pure (seed, tuile)).

## 1. Le principe déjà en place — et c'est le bon

Le monde est reconstructible : `état(cellule) = génération(seed) ⊕ deltas`.
Tout ce qui est dérivable du seed se **jette et se régénère** ; seuls les
**deltas** (ce que le joueur a changé) méritent la mémoire. C'est le
modèle Minecraft/Skyrim, et Meadows l'implémente déjà aux trois quarts :

| Système | Aujourd'hui | Borné ? |
|---|---|---|
| Chunks rendu (terrain/veg/grass) | anneaux + éviction hystérésis (`ChunkStreamer::evictFar`) | ✅ par le rayon |
| Tuiles de génération (S1/érosion) | résidence bornée (12) + **cache disque** (`TerrainBakeStreamer`) | ✅ RAM ; disque ↗ (acceptable) |
| Cellules + références | `unloadCell` détruit les entités ; `PendingSaveLayer::captureCell` capture les deltas AVANT (champ par champ, §5) | ✅ entités ; ⚠️ deltas ↗ RAM |
| PNJ de cellule | despawn avec la cellule, état capturé ; au reload le schedule repositionne selon l'heure | ✅ (contrat à documenter, §4c) |
| Followers | cellule → persistant au recrutement (bornés par la taille du groupe) | ✅ |
| Sculpt / terrain modifié | `HeightPatches` : grille n×n de f32 par chunk touché, map RAM | ❌ **le vrai débordant lourd** |
| GPU (buffers, staging) | VMA + pools ; le ring de staging garde son pire burst (noté) | ✅ mais ne rétrécit pas |
| Quêtes/journal/inventaires | bornés par le contenu authored | ✅ |

Réponses directes aux questions posées : **oui** on décharge quand on
s'éloigne (entités, chunks, tuiles) ; **oui** l'état modifié est capturé
et rejoué au retour (c'est la couche de patches — le reload d'une
cellule = re-résolution avec les deltas par-dessus, prouvé par
`CellDeltaTest`) ; les PNJ despawnent et leur schedule les repositionne
au retour (pas de simulation hors-ring — choix assumé, voir §4c).

## 2. L'empreinte des deltas — ordres de grandeur

- **Patch de référence** (objet déplacé/ramassé/tué) : quelques dizaines
  d'octets (guid + champs modifiés). 10 000 objets touchés ≈ ~1 Mo.
  → aucun besoin d'agir, même sur une très longue partie.
- **Tombstone d'arbre coupé** (§4b) : ~8 octets. Négligeable par
  construction.
- **HeightPatch de sculpt** : n×n f32 par chunk (à n=65 : ~17 Ko/chunk).
  Un joueur qui terraforme partout en accumule sans limite, en RAM, à
  vie de la partie. C'est le seul delta LOURD → §4a.

## 3. Fragmentation — diagnostic avant remède

Les gros cycles alloc/free (payloads de bake, buffers de décode) passent
déjà par des membres réutilisés ou des mailboxes ; le GPU est
sous-alloué par VMA. Les risques restants sont (a) la **croissance sans
retour** (rings/vecteurs qui gardent leur pire burst — staging Vulkan
déjà noté ; les maps de chunks/patches), (b) le churn de petits objets
(nœuds de maps de deltas, strings Lua) sur des heures.

**Position : mesurer avant d'outiller.** Une ligne one-shot façon
« gpu budget » (RSS, taille des maps de patches, compteurs par ring,
haut-niveau d'eau du staging) au HUD perf + log périodique. On ne pose
des arènes/pools que sur un churn prouvé par cette ligne — l'allocateur
système macOS encaisse bien tant que les patterns sont bornés.

## 4. Les trois chantiers qui découlent

### a) Spill disque des deltas lourds (sculpt) — le manquant réel

Le modèle des tuiles s'applique tel quel : `HeightPatches` garde en RAM
les chunks de l'anneau actif + une marge ; au-delà, le patch s'écrit
dans un **fichier région adossé à la save** (clé = (cx,cz), format =
le cooked binaire existant) et se recharge à l'approche. Invariants à
protéger : (1) la couche §5 reste l'unique mécanisme — le fichier région
est un *stockage* de la couche, pas un système parallèle ; (2)
`TerrainParams.patches` reste un sptr immuable publié (le contrat
anti-crash des workers) — le spill publie une nouvelle instance sans
les chunks évincés, comme un republish de TerrainBase.

**Alternative discutable** : tout garder en RAM et ne spiller qu'à la
sauvegarde (plus simple, mais l'empreinte croît avec le terraforming —
acceptable si le sculpt reste un outil d'éditeur plus qu'une mécanique
de jeu). → décision dev selon la place du terraforming dans le design.

### b) Identité des récoltables procéduraux (arbres coupés)

Les arbres ne sont PAS des références : ils sortent du scatter
déterministe (seed, chunk, index) — c'est le golden test. L'identité
est donc **gratuite** : un arbre = (chunk, index de scatter). Couper un
arbre = écrire un **tombstone** `{chunk, index}` dans la couche de
deltas ; au re-scatter du chunk, les index tombstonés sont sautés
(filtrage post-scatter : l'ordre RNG — le contrat — n'est PAS perturbé,
on retire APRÈS tirage). La souche/le bois au sol deviennent des
**références spawnées** ordinaires (le patron guid dérivé
`combine(chunk, index, namespace)` — l'idiome tombe/prefab-child §2.11),
qui vivent la vie normale des références (ramassables, persistées §5).
Coût : ~8 o/arbre, mécanisme 100 % réutilisé. **Voie claire — pas
d'alternative nécessaire.**

### c) Contrat PNJ hors-ring — documenter, pas simuler

Aujourd'hui : despawn + capture + repositionnement par schedule au
retour (l'heure du jour place le PNJ où son emploi du temps le dit).
C'est le modèle Bethesda et il est BON pour l'empreinte (zéro coût
hors-ring). Ce qui manque est un **contrat écrit** : quels champs
survivent (santé ? inventaire ? aggro non — elle expire), et le
« catch-up » borné au reload (pas de simulation rétroactive — le
schedule EST la fonction d'état par l'heure). Alternative lourde
(simulation légère hors-ring type bulle étendue) : à ne considérer que
si un design futur l'exige (caravanes, guerres de factions).

## 5. Ordre proposé

1. **Harness de mesure** (§3) — sans lui, tout le reste est à l'aveugle.
2. **Tombstones récoltables** (§4b) — voie claire, petite, débloque le
   design « couper des arbres ».
3. **Spill sculpt** (§4a) — après décision dev sur la place du
   terraforming ; le harness dira l'urgence réelle.
4. Contrat PNJ (§4c) — documentation + les 2-3 champs à trancher.
