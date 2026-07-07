# Chantier 6 — P1 par valeur : quête, stats avancées, crime, lighting

> **FAIT (2026-07-06) — exécuté d'une traite sur approbation du plan**
> (même mode que les chantiers 2-5). 275 tests / 78 096 assertions verts ;
> smoke-runs en jeu à chaque brique.
> **Axes A/C/D commités ; axe B (lighting) livré NON COMMITÉ dans le
> working tree** — directive dev : validation visuelle d'abord, le
> lighting extérieur actuel (apprécié) ne doit pas bouger sans A/B.
> État global : `docs/MEADOWS-PLAN.md`.

## Périmètre (choisi par le dev, les QUATRE axes)

- **A — Quête jouable + journal** (le dernier P0 de la slice verticale).
- **C — Stats avancées** (l'ex-Phase 9 de `docs/STATS.md` : offensives,
  machine d'état shaken/critical-weakness, encombrement).
- **D — Économie / crime v1** (restock + multiplicateurs marchand, garde,
  prime, amende).
- **B — Passe lighting** (spots, casters d'ombre des meshes, grading
  brique 28, auto-exposition brique 29) — EN DERNIER, non commitée.

Coupés du chantier : démembrement/bleed-out (pas d'assets gore), B2b
(ombre de lumière clé intérieure — stretch descopé, fallback prévu au
plan : B1 ambiance+rampe + SSAO existant).

## A — La quête « La menace de l'est » (commits A1-A4)

- **A1 — OnDeath + tags de faction.** Les `ActorTagForm` deviennent de
  VRAIS GameplayTags sur le système de l'acteur (`registerTag`
  idempotent) ; premier tag `Faction.*` → `npc.factionTag`. Edge
  vivant→mort dans `updateNpcs` → événement `OnDeath{target, factionTag}`
  sur l'EventBus ; `npc.dead` est SEEDÉ depuis le tag au build (un
  cadavre rechargé ne tire pas d'OnDeath parasite).
- **A2 — La quête.** Records Phase-4 purs dans `village.toml` :
  QuestForm + 3 états (hunt/report/Success), branches + tasks
  (`OnDeath` filtré `Faction.Bandits`, puis `OnReportBandit`). Dialogue :
  offre gated « ni Active ni Done », turn-in gated
  `Quest.EasternMenace.Ready` ; récompense (+50 or) dans le handler du
  turn-in (l'option disparaît avec le tag → tire exactement une fois).
  États miroirs en tags joueur (`syncQuestTags`, pattern Phase 4).
- **A3 — Journal (touche J).** `journal.rml` (data-model `journal`,
  rows titre/objectif/n-m), ScreenJournal modal dans `ui.toml`.
  `platform::Key::J` ajouté (append à l'enum — purge des *.dir faite).
- **A4 — Persistance quêtes + console.** `SavedQuestForm` /
  `SavedQuestTaskForm` (guids déterministes `combine(kSavedQuestNs,…)`),
  `captureQuestLog` reconstruit du QuestLog vivant à chaque save,
  `applySavedQuests` après resolve si `loadedFromSave` (pattern
  WorldStateForm — le QuestLog est scène-niveau, PAS par-acteur).
  Console : `startquest <editorId>`, `queststate`. Doctest round-trip
  (QuestSaveTest).

## C — Stats avancées (commits C1-C3)

- **C1 — Stats offensives + crits.** Dérivées : `attack` (5+force),
  `criticalDamage` (constante de tuning — la formule de STATS.md §3
  `1.5+(dexterity−0.5)` est ambiguë, voir « à trancher »),
  `armorPenetration`/`resistPenetration` ((attr−5)·0.5, plancher 0).
  `weaponDamageEvent` plie `attack` dans le canal physique le plus fort
  (canal Blunt pour les poings) et porte pens + multiplicateur sur le
  `DamageEvent`. `applyDamage` : les pens ne réduisent QUE la protection
  positive (jamais d'amplification d'une vulnérabilité) ; exécution
  critique = criticalSensitivity% de la maxHealth de la CIBLE ×
  multiplicateur, ignore l'armure, quand `tryPlayerAttack` voit
  `State.CriticalWeakness` sur la cible. 8 constantes APPEND sur
  `StatsTuningForm`. Doctest : OffensiveStatsTest.
- **C2 — Machine d'état : CriticalWeakness + Shaken.** Le break de
  posture n'est plus « restaure à max » : il ouvre la FENÊTRE CRITIQUE —
  `State.CriticalWeakness`, posture à 0 pendant `critWindowSeconds`
  (5 s), restauration à l'expiration (`updateCritWindow`, miroir
  d'updateStagger ; le regen de posture est gated sur la fenêtre).
  Coup de posture lourd sans break (> (15+constitution)% de maxPosture)
  → `State.Shaken` bref. **APPENDs consolidés en UNE brique** :
  `CombatState` += critWindowSeconds/shakenSeconds ; `SavedStatsForm` +=
  les mêmes + lastRestockHours + bounty ; nouveaux composants réfléchis
  `VendorState`/`Bounty` (`gameplay/actors/ActorState.hpp`) balayés par
  captureActor/applySavedState.
- **C3 — Encombrement + jump power.** Dérivées `maxEncumbrance`
  (50+for·10+con·2) et `jumpPower` (80+grâce+dex+for·2), en CURRENT
  (fortify-carry marche tout seul). `inventoryWeight` sur tous les types
  d'items ; catégories léger/moyen/lourd/surchargé avec pénalités
  vitesse/accél pliées en ×-modifiers au site equipMods de la scène
  (§2.9 : jamais d'écriture directe). Saut = jumpPower × kJumpScale3D
  (feuille par défaut = les 5.0 m/s d'avant) ; surchargé = ni saut ni
  sprint ; le footer d'inventaire affiche poids / max (catégorie).

## D — Économie / crime v1 (commits D1-D2)

- **D1 — Marchand.** `ActorForm` APPEND `buyMult`/`sellMult` (0 = tuning
  global) — le Villager-marchand a son profil (1.4/0.55) dans
  `village.toml`. Restock à l'ouverture du barter si > 24 h de temps de
  jeu depuis le dernier re-roll : clear + `applyLoadout` (l'or re-roule
  avec, comportement Skyrim assumé). L'horloge vit dans le composant
  réfléchi `VendorState` (capturé par la save — pas de map de scène =
  pas d'exploit de restock au reload).
- **D2 — Crime.** PNJ Garde (`village.toml` : ActorForm +
  `Faction.VillageGuard` + schedule guard 24 h + réf sur la place +
  dialogue). Frapper un PNJ paisible devant témoin (la victime vivante,
  ou tout PNJ vivant < 20 m avec LOS raycast vers le joueur — l'idiome
  B5) → +40 de prime sur le composant `Bounty`, mirroré en tag
  `Crime.Wanted` (`syncWantedTag` — les conditions ne voient pas les
  composants, pattern sanctionné CLAUDE.md §6.1). Les gardes deviennent
  hostiles tant que Wanted (extension du branch npc.hostile, base tags —
  la table de relations reste pour plus tard). Option de dialogue du
  garde « payer l'amende (40 or) » gated HasTag Crime.Wanted + HasItem
  or ≥ 40 → retire l'or, clear prime + tag. `finalizeActorSpawn` seed
  Bounty/VendorState sur tout acteur pour que les champs name-matchés de
  la save atterrissent au load ; `syncWantedTag` re-mirrore après load.

## B — Passe lighting (NON COMMITÉE — working tree, toggles A/B)

- **B1 — Spots + falloff + ambiance intérieure.** `SceneLight` +=
  direction (Transform.rotation × +Z, la convention yaw de la scène) +
  spotAngle ; `uLightDirectionAngle[16]` APPEND en FIN de LightsUbo des
  DEUX côtés (la leçon UBO) — w = cos(demi-angle), −2 = point light.
  Falloff quintique stylisé (smootherstep) + cône spot à bord doux 10 %
  dans `locallights.glsl`. `interiorAmbient` (Vec3) APPEND sur
  `LandscapeTuningForm` + `landscape.toml` (remplace le {0.16,0.15,0.14}
  codé en dur). Retune village : lanterne à flicker 0.12 (flamme
  enclose) vs bougies 0.45, fill froid côté fenêtre, premier spot
  (`kind = "spot"`, TableSpot pitché vers le bas).
- **B2a — Casters mesh dans le CSM soleil.** `shadow_mesh.vert` /
  `shadow_skinned.vert` depth-only (CasterModelUbo binding 4 — jamais en
  collision avec ShadowUbo binding 1) ; les meshes du snapshot ET les
  PNJ skinnés sont dessinés dans les 3 passes cascades (UBO modèle
  réutilisés, 1 frame de retard pour les PNJ — invisible en 2048²).
  Toggle « Mesh shadow casters » (défaut ON — c'est LA correction
  extérieure attendue : les maisons projettent enfin).
- **B3 — Brique 28 : grading analytique.** Dans `tonemap.frag` entre
  acesFilm et le gamma : vibrance pondérée, split-toning (ombres
  fraîches/hautes chaudes), contraste pivot 0.5. Paramètres APPEND sur
  `LandscapeTuningForm` + `landscape.toml`, transportés sur les slots .w
  libres (uSunGlowColor/uZenithColor/uHorizonColor). Toggle « Grading
  (brick 28) » **défaut OFF** + 3 sliders.
- **B4 — Brique 29 : auto-exposition.** `PostFx` : log-luminance R16F
  64² → `generateMipmaps` → moyenne 1×1 ; micro-passe `adapt.frag`
  ping-pong 1×1 (inertie asymétrique — l'obscurité adapte plus
  lentement ; snap au premier frame) ; le tonemap tape la texture
  (binding 5, un blit group par côté du ping-pong) × le slider Exposure
  devenu biais EV. Bornes min/max APPEND tuning + TOML. Toggle « Auto
  exposure (brick 29) » **défaut OFF**.
- **B2b — descopé** (stretch) : l'ombre perspective de lumière clé
  intérieure. Fallback assumé du plan : B1 (ambiance + rampe) + SSAO.
- **B5 — passe intérieure (post-chantier, retour dev « le lighting
  intérieur est vraiment moche », 2026-07-07).** Deux corrections gated
  sur un flag intérieur (`uCascadeSplits.w`, extérieur byte-identique) :
  (1) **ambiante hémisphérique** dans mesh/skinned.frag — sols clairs et
  frais, plafonds sombres et chauds (tue le gris constant plat, la cause
  n°1) ; (2) **wrap half-Lambert + bounce** dans locallights.glsl — le
  terminateur des bougies s'adoucit et chaque lumière ajoute un petit
  remplissage sans N·L : la pièce prend la teinte de ses sources
  (stand-in du premier rebond de GI). Constantes dans les shaders =
  réglables à chaud (hot reload sur l'arbre source). Si les intérieurs
  restent en deçà après tuning : B2b (ombre de lumière clé) est la
  brique suivante — elle stoppe aussi le light bleed à travers les murs.
  Contexte : audit Community Shaders (GPL — idées oui, code non) —
  retenus pour plus tard : screen-space shadows (contact), wetness +
  occlusion de pluie top-down (spec brique 31), ombres de terrain
  lointaines via nos cartes d'horizon, skylighting, A/B falloff
  inverse-square.

### Fichiers B (à commiter APRÈS validation visuelle dev)

`engine/render/landscape/shaders/{locallights.glsl, tonemap.frag,
shadow_mesh.vert/frag, shadow_skinned.vert/frag, luminance.frag,
adapt.frag}`, `engine/render/landscape/PostFx.{hpp,cpp}`,
`data/forms/LandscapeForms.hpp`, `game/SceneSubmit.{hpp,cpp}`,
`game/scenes/LandscapeScene.{hpp,cpp}`,
`game/data/base/{landscape.toml, village.toml}` (retune lumières).

## À trancher (dev)

- **Formule criticalDamage** : STATS.md §3 dit « 1.5 + (dexterity−0.5) »
  — ×7 à dex 6, sûrement une typo. Implémenté : 1.5 + dex·0.05
  (constante `critDamagePerDexterity`, retunable en TOML sans recompile).
  Corriger STATS.md dans un sens ou l'autre.
- Les toggles B : si le grading/l'auto-expo plaisent, choisir les
  valeurs par défaut et commiter ; sinon ajuster les sliders et reporter
  les valeurs dans `landscape.toml`.

## Pièges payés / leçons

- **PS -replace sur un fichier source = mojibake** (déjà payé au
  chantier 4, re-payé ici sur village.toml : Get-Content -Raw décode
  l'UTF-8 sans BOM en ANSI). Restauré via git, ré-appliqué à l'outil
  Edit. Règle ferme : jamais de réécriture regex PowerShell sur les
  sources.
- **Guillemets doubles dans un message de commit** : PowerShell 5.1
  mange les `"` imbriqués en les passant à git → message éclaté en
  pathspecs. Pas de `"` dans les -m.
- Purges des *.dir faites à CHAQUE layout de type partagé (Key::J,
  StatsTuningForm ×2, CombatState/SavedStatsForm, ActorForm,
  LandscapeTuningForm ×2) — zéro crash stale-obj sur ce chantier.
- `uTerrainInfo.w` et `uPostInfo.z` sont PRIS (reflectionsActive,
  cascadeDebug) — les slots libres réellement utilisés : uSunDirection.w
  (dt), uWindInfo.w (flag auto-expo), uHorizonFarColor.w (min),
  uCloudMapInfo.w (max), et les .w de sunGlow/zenith/horizon (grade).

## Quoi tester (dev)

1. **Quête** : parler au Villager → « Puis-je aider le village ? » →
   J (journal) → tuer le bandit à l'est → retour, turn-in → +50 or,
   quête Succeeded au journal. F5 au milieu, F9 : l'étape est conservée.
2. **Crits** : break de posture sur le bandit (arme à postureDamage) →
   il reste posture 0 ~5 s → frapper pendant la fenêtre → « CRITICAL! »
   dans le log, gros dégâts.
3. **Encombrement** : looter lourd → footer inventaire (catégorie) →
   surchargé = lent, ni saut ni sprint.
4. **Marchand** : acheter/vendre (prix au profil 1.4/0.55), dormir 24 h+
   (repos), rouvrir le barter → stock re-roulé (log « Vendor
   restocked »).
5. **Crime** : frapper le Villager devant le garde → « Crime observe ! »,
   garde hostile → lui parler → payer l'amende (40 or) → calme. F5/F9
   avec prime active : toujours Wanted au load.
6. **Lighting (B, non commité)** : intérieur = bougies qui dansent vs
   lanterne stable, fill froid, spot au-dessus de la table ; extérieur =
   les maisons/PNJ projettent (toggle « Mesh shadow casters ») ;
   « Grading (brick 28) » ON → verts plus riches, ombres fraîches ;
   « Auto exposure (brick 29) » ON → entrer dans la maison à midi :
   l'œil s'adapte en ~2-4 s ; ressortir : ré-adaptation rapide.
