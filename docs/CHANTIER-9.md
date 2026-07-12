# Chantier 9 — Confort & plateforme (journal)

> Exécuté le 2026-07-12 (plan approuvé le même jour). Toutes les briques
> livrées ; jalons dev : manette (C9.4) et bascule de langue (C9.5)
> VALIDÉS en jeu, carte (C9.6) livrée « à garder pour le moment »
> (palette retouchable dans le bloc [cpp-tuning] de game/MapRaster.cpp).

## Briques livrées

| Brique | Commit | Contenu |
|---|---|---|
| C9.1 canal manette | `f9c6434` | `platform::Input` : PadButton (gâchettes = boutons logiques au seuil), hotplug SDL_Gamepad (un pad actif), sticks à deadzone radiale PURE (doctestée), `moveAxis` retombe sur le stick gauche. |
| C9.2 couche d'actions | `3838313` | `game/InputActions` (InputAction + ActionMap, rebind = VOL à l'ancien propriétaire) + `game/Settings` (settings.toml à côté de saves/ — préférence machine, PAS un Form). ~12 sites migrés ; look = souris × sens + stick droit rad/s × dt, un seul invert Y. |
| C9.3 nav UI manette | `0866dac` | RmlUi 6.1 : `tab-index`/`nav` en RCSS (pas d'attribut en 6.1), sorties `#id` des scroll containers (le spatial n'y descend pas), `:focus-visible` doré ; façade focusFirst/activateFocused ; d-pad/stick→flèches, A=valider, B=closeTop ; gardes anti double-feu pad UI↔gameplay. |
| C9.4 options + rebind 🎮 | `819145b` | ScreenOptions : sensibilités/deadzone/invert steppers, volume master (= setBusVolume ×5 bus), bindings par CAPTURE (decideCapture pur doctesté ; Échap annule sans ouvrir la pause ; B/Start capturables). VALIDÉ dev. |
| C9.5 localisation 🎮 | `78b3483` | Base EN (`text-en`, 113→116 clés) + pack FR = patches champ `text` (§5) gaté par settings.language AVANT la stack ; passe `data-loc` (UiSystem.setLocalizer/relocalize, callback — meadows-ui sans data/) ; format N args ; cooker `import-csv --patch` ; bascule EN↔FR EN DIRECT (stack re-résolue dans une FormDatabase TEMPORAIRE → texts.build + relocalize — aucun pointeur de Form vivant ne bouge). VALIDÉ dev. |
| C9.6 carte 🎮 | `ec4c4c0` | `game/MapRaster` : raster 512² CPU depuis terrain::height/materialWeights (+patches .ter), extent = bbox des CellForms (cellSize 64) ; façade texture runtime:// (RmlUi `<img>` natif ; placeholder 1×1 seedé AVANT le preload — un échec de chargement lazy se lattche à vie) ; bake sur le JobSystem (~261 ms Debug). Marqueur joueur + POI portes ; intérieurs = overworld sans marqueur. |
| C9.7 save async | `006baa1` | Capture sur la frame, sérialisation+écriture sur worker (idiome ResidencyCache), .tmp+rename atomique, SaveFlightGate (single-flight pur doctesté), toast à complétion avec capture/serialize/write ms ; timings du load (parse/resolve/rebuild) loggés. |
| C9.8 plateforme | (ce commit) | `platform::localTime` portable (localtime_s = MSVC-only, ×2 sites) ; checklist Fedora ci-dessous ; décision macOS inscrite (MEADOWS-PLAN + CLAUDE.md §2.1). |

## Décision macOS (dev 2026-07-12)

**Le chemin macOS = backend Vulkan + MoltenVK, chantier dédié POST-DÉMO.**
- PAS de down-port GL 4.1 du renderer 3D : l'API Apple est dépréciée et
  figée ; compute (Hi-Z, RC GI), SSBO (skinning), DSA et bindless en
  sont absents — ce serait un second renderer amputé. La version 2D
  d'origine reste le legacy 4.1, gelée.
- PAS de backend Metal natif (3e API pour la plus petite audience) ;
  SPIRV-Cross permettrait d'en dériver un plus tard si le besoin
  devient réel.
- Discipline immédiate : tout nouveau code RHI derrière les capability
  flags, zéro GLisme hors `rhi/backends/gl`.

## Checklist de vérification Fedora (à dérouler par le dev, §3.1)

L'audit de portabilité (exploration C9) n'a trouvé QU'UNE casse
certaine — `localtime_s`, corrigée ici. Le reste est propre (zéro
windows.h, zéro souci de casse d'includes, zéro backslash, CMake
branché MSVC/GCC, imgui-node-editor vendored correctement gardé).
Sur la machine Fedora :

1. `cmake -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Debug` (clang ou
   gcc — CPM télécharge les deps identiques).
2. `cmake --build build-linux --target meadows-tests true-adventurer
   cooker` — points d'attention : le LINK SDL3 statique (aucun
   `SDL_main.h` n'est inclus dans game/main.cpp — si le link échoue sur
   `main`, c'est là) ; toml++ en mode TOML_EXCEPTIONS=0 (déjà défini
   par cible) ; l'entry GL (glad) sous X11/Wayland.
3. `./build-linux/tests/meadows-tests` — même compte que Windows.
4. Lancer le jeu : vérifier gamepad (SDL3 gère udev), settings.toml et
   saves/ à côté de l'exécutable, la bascule de langue, la carte.
5. Toute casse trouvée = mini-brique de suite (ce journal la liste).

## Restes / notes

- Conteneur/barter : le pad navigue la liste principale ; la traversée
  spatiale vers le panneau droit est bloquée par le scoping RmlUi des
  scroll containers (B/Take-all couvrent le flux).
- La palette de la carte attend la retouche visuelle du dev
  ([cpp-tuning] en tête de MapRaster.cpp) ; promotion en données
  possible si le dev veut itérer seul.
- Volume master = écriture des 5 bus (pas de vrai master mixer) — su
  et documenté dans le code.
