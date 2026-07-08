# Chantier — Cellules extérieures implicites ("éditer partout")

> Plan d'un chantier vertical de `docs/MEADOWS-PLAN.md`. Décidé avec le dev
> (2026-07-08) : on veut pouvoir **éditer/modifier le jeu n'importe où** sur le
> terrain. À lire avant de toucher `world/worldspace/`, `world/streaming/`, ou
> le placement de l'éditeur (`game/scenes/SceneEditor.cpp`).

---

## 1. Le problème

Placer un objet dans l'éditeur exige une **cellule autorisée** au point cliqué
(`SceneEditor::draw` → `worldModel.cellAt` → sinon *"no authored cell here"*).
L'Overworld (`adventure.toml`, `cellSize` 64) n'autorise que **3 cellules**
(gridX 0/1/2, gridY 5 = la bande du village). Partout ailleurs → refus.

Asymétrie avec le terrain, qui, lui, est éditable **partout** : sculpter crée un
`HeightPatch` **à la demande** pour n'importe quel chunk `(cx,cz)` (pure case de
tableau, adressée par coordonnée). Une **cellule**, elle, est aujourd'hui une
`Form` adressée par **handle** (index dans la `FormDatabase` résolue), produite
par les plugins au chargement — jamais créée à la volée.

## 2. La décision : cellules extérieures implicites, matérialisées à la pose

On adopte le **modèle open-world Bethesda** (déjà l'esprit du projet) : une
cellule d'extérieur n'est pas « autorisée », c'est une **case d'une grille
infinie adressée par `(worldspace, gx, gy)`** — exactement comme un chunk de
terrain.

- **Identité déterministe** : le GUID d'une cellule extérieure se **dérive** de
  `(worldspace GUID, gx, gy)` (hash → GUID). La même case a toujours la même
  identité, quelle que soit l'ordre de chargement (respecte §2.5 : identité par
  GUID stable, jamais par position de load).
- **Matérialisation paresseuse** : tant que rien n'y est posé, la cellule est
  **virtuelle** (aucun enregistrement, coût nul — le streamer la saute déjà).
  À la **première pose d'un objet**, on **matérialise** la cellule : on crée un
  `CellForm` avec ce GUID déterministe, on l'ajoute *live* à la base et on
  l'écrit dans le plugin de l'éditeur (export).

Les deux grilles deviennent symétriques :

| | Terrain (fait) | Objets (ce chantier) |
|---|---|---|
| Adressage | `(cx,cz)` implicite, infini | `(gx,gy)` implicite, infini |
| Matérialisation | patch créé au coup de pinceau | cellule créée à la pose |
| Enregistrement | `TerrainPatchForm` au save | `CellForm` au placement |

**Intérieurs comme extérieurs.** La machinerie cellule/streaming est
**uniforme** : `performTravel` fait `unloadAll` puis `cellStreamer->update` sur
le worldspace d'intérieur exactement comme en extérieur ; l'intérieur ne diffère
que par `interiorMode` (le sol est le plan `y=0`, pas le terrain). Donc la
matérialisation est **agnostique du worldspace** : la `CellForm` créée hérite
simplement du flag `interior` de son worldspace. Résultat :

- **Extérieur** : grille infinie, matérialisée à la pose — « éditer partout ».
- **Intérieur** : éditer *dans* une cellule autorisée marche déjà (une pièce
  tient dans une cellule de 64 m) ; la matérialisation permet en plus de
  **construire au-delà** (grandes salles, donjons multi-cellules) sans autoriser
  les cellules à la main.

**Hors de ce chantier** : *créer un nouvel intérieur* (nouveau `WorldspaceForm` +
porte/travel câblée) — c'est un chantier « éditeur de worldspaces & portes »
distinct. Ici on édite *dans* des worldspaces existants (intérieur ou extérieur).

## 3. Ce qui existe déjà (le chantier est petit)

- **`FormDatabase::add(uptr<Form>, type)`** — insertion *live*, renvoie un
  handle. On peut donc matérialiser une cellule sans recharger. C'est
  l'activateur central.
- **`CellStreamer::update` boucle déjà sur la grille** autour du focus et
  appelle `model.cellAt(worldspace, x, y)` ; il charge ce qui a un handle valide
  et saute le reste (« Cells that have no CellForm record simply don't exist »).
  → **aucun changement de streaming** : dès qu'une cellule est matérialisée, le
  streamer la charge tout seul.
- **`WorldspaceForm.interior`** existe → extérieur = `!interior`, pas de nouveau
  champ.
- **`CellLoader::loadCell(handle)`** crée l'entité de cellule et spawn ses
  références ; **`cellEntity(handle)`** la retrouve. Inchangés.
- **`core::fnv1a`** pour dériver l'identité de façon stable.

## 4. Invariants respectés

- **§2.5** — GUID déterministe dérivé de `(worldspace, gx, gy)`, jamais de
  l'ordre de load. Deux sessions/mods qui touchent la même case parlent de la
  même cellule.
- **§5** — la cellule matérialisée est un **enregistrement ordinaire** dans le
  mod de l'éditeur (`level-edits.toml`), superposé comme n'importe quel plugin.
  Aucun mécanisme parallèle. Le save reste une couche de patches par-dessus.
- **§2.2 / §2.7** — cellule = `CellForm` (Form), référence = `ReferenceForm`
  placée dans la cellule ; le spawner et le streaming ne changent pas.
- Le lien référence→cellule (`ReferenceForm.cell`) est **inchangé** ; save et
  streaming continuent de fonctionner par cellule.

## 5. Plan brique par brique

### Brique 1 — Identité déterministe + matérialisation dans WorldModel
- `world::cellGuidFor(const core::Guid& worldspace, i32 gx, i32 gy) → core::Guid`
  déterministe (dérivé des octets du GUID worldspace + gx/gy).
- `WorldModel::materializeCell(FormDatabase&, worldspace, gx, gy) → FormHandle` :
  idempotent — si `handleOf(cellGuidFor(...))` existe, le renvoyer ; sinon créer
  un `CellForm` (`worldspace`, `gridX`, `gridY`, `interior` = celui du
  worldspace), `forms.add`, puis mettre à jour `cellByCoords` +
  `worldspaceByCell`. Renvoie le handle. **Agnostique intérieur/extérieur.**
- **Tests headless** (obligatoires, §8) : matérialiser deux fois la même case →
  même GUID/handle ; `cellAt` la retrouve ; une case extérieure non matérialisée
  reste invalide.

### Brique 2 — L'éditeur pose n'importe où (matérialise + exporte)
- `LevelEditor::ensureCell(worldspace, gx, gy) → core::Guid` : appelle la
  matérialisation live (brique 1) **et** enregistre le `CellForm` dans
  l'`EditSession` avec **le GUID déterministe** (pour l'export). *Point à
  vérifier : `EditSession::createForm` doit pouvoir imposer un GUID donné ; sinon
  petite extension.*
- `SceneEditor::draw` (placement) : remplacer « `cellAt` sinon abort » par
  `ensureCell(...)`, puis charger la cellule (`cellLoader.loadCell` si pas
  résidente, en la marquant résidente côté streamer pour éviter le double-load),
  puis spawn la référence comme aujourd'hui.
- **Validation en jeu** : poser un objet en pleine nature → il reste ; Export →
  le mod contient le nouveau `CellForm` + la `ReferenceForm` ; relance → tout se
  recharge proprement (WorldModel.build reprend le `CellForm` du plugin, même
  GUID).

### Brique 3 — Robustesse save/reload + coordination streamer
- Vérifier qu'une cellule matérialisée puis sauvée se recharge (elle est dans le
  mod ; le save est une couche par-dessus — §5).
- Coordination `loadCell` éditeur ↔ `resident` du `CellStreamer` (une seule
  entité de cellule ; pas de double spawn). Idempotence du spawn.
- Cas limite : matérialiser une case déjà dans le rayon de streaming.

### Brique 4 (plus tard, si besoin) — Grille pleinement virtuelle
- Rendre les cases **vides** adressables/chargeables (contenu procédural par
  cellule : scatter d'objets, rencontres…). **Pas nécessaire pour éditer
  partout** — la matérialisation paresseuse suffit. À ne faire que si un besoin
  concret apparaît (le scatter herbe/végétation passe aujourd'hui par les
  systèmes terrain, pas par les cellules).

## 6. Questions ouvertes à trancher en implémentant

1. **`EditSession` + GUID imposé** : `createForm` mint-il un GUID aléatoire ? Si
   oui, ajouter un `createFormWithGuid` (ou paramètre) — la brique 2 en dépend.
2. **Live `forms.add` vs export** : la cellule live (cette session) et la cellule
   du mod (au reload) portent le même GUID déterministe → pas de doublon au
   reload (base reconstruite de zéro). À confirmer par le test de la brique 2.
3. **Où vit `cellGuidFor`** : `world/worldspace/` (à côté de `WorldModel` /
   `CellForm`), réutilisable par l'éditeur et un futur générateur procédural.
4. **Marqueur "matérialisée"** : suffit-il d'un `CellForm` ordinaire, ou faut-il
   distinguer autorisée-à-la-main vs matérialisée ? A priori non — une cellule
   est une cellule (§2.2). À garder simple.
