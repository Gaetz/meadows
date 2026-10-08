[← Back to the hub](README.md)

# In-game tools

## The Game DB editor

Open **Game DB (editor)** from the menu. It shows the ENTIRE resolved game
database — every record of every type, from every enabled plugin.

- **Browse**: filter by type, search by `editorId`.
- **Edit**: select a record — every field appears in the property panel
  (the panel is generated from the data model itself, so new fields and
  types show up automatically). Full undo/redo.
- **Create**: pick a type, hit *New* — a fresh GUID is minted for you.
- **Export plugin**: your pending edits become a standard `.toml` mod
  file, added to the load order, containing ONLY what you changed. This
  is the fastest way to make a tweak mod: edit, export, done. Edits apply
  after *Reload data* (or a relaunch).
- **Anim Preview** (Windows menu): select an anim clip or an anim graph —
  it plays on its skinned mesh in 3D (drag to orbit, wheel to zoom). A
  clip scrubs with its timeline events highlighted; a graph runs LIVE
  with sliders for its parameters and checkboxes for its tag gates, so
  you can trigger transitions and watch the cross-fades. Your unsaved
  edits preview immediately.

## The level editor (in the 3D scene)

Press **F6** in the Landscape scene. Everything you do writes RECORDS,
and *Export* saves them as an ordinary mod (`data/mods/level-edits.toml`)
loaded on the next run — your level edits ARE a plugin.

- **Pick**: left-click an object. **Gizmo**: move/rotate/scale with the
  handles; `1`/`2`/`3` switch the operation. The record is patched when
  you release. Tick **Snap** to move on a metric grid (step configurable)
  and rotate on a 15° lattice.
- **Duplicate**: with an object selected, the *Duplicate* button (or
  **Ctrl+D**) clones it one meter aside — one Ctrl+Z removes the copy.
- **Place**: arm a palette entry (Statics / Lights / Prefabs), click the
  ground. `Esc` cancels.
- **Group into a prefab**: Ctrl+click several objects, name it, *Create
  prefab from group* — the originals collapse into one reusable prefab
  you can place again from the palette.
- **Sculpt terrain**: check *Sculpt terrain*, choose Raise/Lower/Flatten/
  Smooth, paint with the left button. *Save terrain to mod* stages the
  grids; *Export* writes them with the rest.

### First steps, in order

1. Open the **Landscape (3D)** scene and press **F6** — the *Level
   editor* window appears (if not: F10 un-hides the panels).
2. **Move an object**: left-click a rock. Colored gizmo handles appear on
   it — drag them. Press `2` to rotate, `3` to scale, `1` back to move.
   Release the mouse: the change is recorded.
3. **Sculpt**: in the Level editor window, tick **Sculpt terrain**. Now
   the left mouse button is a brush — hold it on the ground and the
   terrain rises (pick *Lower*, *Flatten* or *Smooth* in the *Brush*
   combo; *Radius*/*Strength* sliders tune it). Grass, props and
   collision follow when you release. Untick *Sculpt terrain* to go back
   to selecting objects.
4. **Keep your work**: *Save terrain to mod* (if you sculpted), then
   **Export mod** — everything lands in `data/mods/level-edits.toml` and
   is loaded automatically on the next launch.
5. To fly while editing: hold the **left button on the sky/ground far
   away won't select** — use LMB-hold **+ WASD** like the normal fly
   camera (the camera only captures the mouse while you hold and move).

## The plugin manager

The **Plugins** window shows the load order: reorder with the arrows,
enable/disable with the checkboxes, *Save order* writes `plugins.toml`.
Below it, the **conflict report** lists every field written by more than
one plugin, with the writers in order — the last one is what the game
uses ([why](load-order.md)).

## The developer console

The **Console** window understands:

| Command | Effect |
|---|---|
| `help` | list commands |
| `find <text>` | list records whose editorId matches |
| `get <EditorId>.<field>` | read any field of any record |
| `set <EditorId>.<field> <value>` | edit it (becomes a pending edit — export to keep it) |
| `undo` / `redo` | edit history |
| anything else | runs as **Lua** on the game's script VM |

`set` goes through the same edit session as the Game DB editor: console
tweaks export to a plugin exactly the same way.

The IN-GAME console (F8, Landscape scene) adds `spawn`, `tp`, `tgm`,
`settime`, `startquest`, and `setstage <quest> <state>` (jump a quest to
any of its states by editorId).

## Checking a mod: `cooker validate`

The command-line cooker can lint a load order before you ship it:

```
cooker validate base.toml my-mod.toml
```

It resolves the plugins IN THAT ORDER and reports:

- **errors** (exit code 1 — fix before shipping): patches to records
  nothing creates, missing/misordered dependencies, and **dangling
  references** — any GUID field pointing at a record or asset that no
  listed plugin provides;
- **information**: per-field conflicts. Two plugins writing the same
  field is normal layering (the last one wins) — the report just shows
  who wrote what, in order.

Validate your mod TOGETHER with the plugins it builds on, or references
into them will (correctly) show up as dangling.

Related: [How plugins work](plugins.md) ·
[Load order & conflicts](load-order.md)

## Le pupitre de génération (sandbox)

Tous les réglages de la génération des cartes sandbox (couche monde,
rythme, plan de points d'intérêt, synthèse macro, érosion) sont un Form
`TerrainGenTuningForm` : un record unique, patchable par n'importe quel
plugin (§5, dernier écrivain gagne par champ). Trois façons de le régler :

- **En jeu, mode Édition → panneau « Terrain generation » → « Generation
  pupitre »** : éditez les champs (filtre `world`, `rhythm`, `poi`,
  `macro`, `bake`), **Apply & re-bake map** re-cuit la carte active avec
  ces valeurs (≈ 40 s en Release, derrière le voile), **Save overlay**
  écrit `data/mods/terrain-gen.toml` (chargé au prochain lancement),
  **Save preset / Load preset** gèrent `data/mods/terrain-gen-presets/`.
- **À la main** : un plugin qui patche le record
  `1a4d5c00-0000-4000-8000-00000000000a` (type `TerrainGenTuningForm`).
- **Hors jeu** : `cooker landscape-report <gameDir> [mapX mapZ]` cuit la
  carte si son cache est périmé, écrit `plan.png` à côté des tranches et
  imprime le recensement du paysage (pente, parts de pas > 30°/45°, murs
  par km, relief par 250 m, sol à plus de 100 m, lacs, rivières) ; une
  ligne d'historique par exécution dans `terrain-cache/<seed>/landscape-report.log`.

Le cache des cartes est clé sur les valeurs du pupitre : changer un
réglage invalide la carte, qui se re-cuit à la demande (le jeu, le cooker
et les tests partagent le même mappage `game::makeTerrainBakeParams`).

### La table d'archétypes des zones

Le sandbox est une mosaïque de zones d'environ 1 km, chacune d'un
archétype (prairie, bocage, collines, replat boisé, plateau-mesa, haut
plateau, bassin, marais, badlands, pierrier, lande, crête-massif, falaises
de côte, côte basse). La table est faite de records `ZoneArchetypeForm`
dans `base/landscape.toml` (un par archétype, ordonnés par `rank`) : les
conditions (`minEtage`/`maxEtage`, `minMassif`/`maxMassif`,
`minCoast`/`maxCoast`, `minMoisture`/`maxMoisture`, lues sur la couche
monde au centre de la zone), le `weight` du tirage, le `storeyBias`
(paliers ajoutés : +1 plateau, −1 bassin) et la grammaire (`reliefMul`,
`wavelengthMul`, `terrace`, `cliffStep`, `hillCrests`, `hardBias`,
`wetBias`, `coverBias`, `palette`, et la petite forme `piece` : 1 butte,
2 clairière, 3 mare, 4 bloc rocheux, 5 bosquet, avec `pieceHeight` et
`pieceRadius`, et `erosionCut` : le budget d'érosion de l'intérieur de la
zone en mètres, 0 = socle brut, 200 = versants disséqués). Un mod peut patcher une ligne (§5) ou en créer une
nouvelle ; sans aucun record, le jeu utilise la même table en C++.
Les réglages de la mosaïque elle-même (taille de cellule, pas de palier,
nombre de paliers, tendance longue, largeur des murs, rampe des
corridors) sont dans le pupitre, groupe `zone`. Les lignes de carte de
style « crêtes » traversent une rangée de zones **rempart** : jamais en
dessous d'une voisine (la chaîne de bordure naît sur une ligne de
partage des eaux, aucune rivière ne longe la frontière) ;
`zoneRampartSteps` ajoute des paliers à cette rangée (0 par défaut).

**Murs et portes.** Deux zones d'étages différents sont séparées par un
mur : une **bande de falaise** pour un palier (franchissable à ses
**encoches**), un **escarpement** pour deux paliers ou plus (percé de
**brèches-rampes**) ; deux zones de massif de même étage se rejoignent
sur une **crête** (`zoneRidgeHeight`, 30 m, 0 = aucune) coupée à ses
**cols**. Chaque frontière a une porte, parfois deux (`zoneGateExtra`,
probabilité de la seconde) : un couloir de `zoneGateHalfWidth` (60 m de
demi-largeur, ×1,5 pour une brèche) qui rampe le palier sur
`zoneRampWidth`, sans falaise, avec un replat de révélation en haut de
la rampe. Le rapport `landscape-report` compte les portes par type et
les tronçons de mur sans passage (aucun ne devrait dépasser 400 m) ; le
`plan.png` dessine les murs en sombre, les portes en blanc, les crêtes
en violet.

