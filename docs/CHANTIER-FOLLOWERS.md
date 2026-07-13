# Chantier FOLLOWERS — plan d'implémentation par étapes (réutilisation d'abord)

> Planifié le 2026-07-12 ; **EXÉCUTÉ dans la nuit du 12 au 13** — toutes
> les étapes É0→É10 LIVRÉES + É11 v1 (preuve de tech monture), un commit
> par étape : É0 `bbfe01a`, É1 `f04da89`, É2 `5acdf94`, É3 `d96422d`,
> É4 `09483b6`, É5 `f063be8`, É6 `ba6384e`, É7 `9af8f93`, É8 `2a99c38`,
> É9 `550cffb`, É10 `ce04ca2`, É11v1 `89418d7`. Suite 445 → 497 cas /
> 81 760 assertions. **Validation dev en jeu = `FOLLOWERS-TEST.md`**
> (protocole complet, réserves v1 déclarées par étape). Restes connus :
> porter la dépouille (É8b, mécanique de placement), radial des
> consignes, matrice d'affinité dynamique, persistance des horloges
> anti-répétition, la suite montures (sifflet/écurie/anim/combat), axe
> réputation des prix mercenaires. Design source : `docs/FOLLOWERS.md`.

## Contexte

Le dev a écrit `docs/FOLLOWERS.md` (380 l. : compagnons persistants,
relation/affinité, classes+niveaux, survie/blessures partagées avec le
joueur, pouvoirs spéciaux, perks réciproques, équipement, mort/tombe,
multi-followers, mercenaires, montures). Demande : un plan
d'implémentation par étapes qui **RÉUTILISE LES SYSTÈMES EXISTANTS**
(insistance dev — à inscrire aussi dans CLAUDE.md comme contrainte).
Le plan lui-même est le livrable prioritaire : il doit être
suffisamment complet pour être exécuté dans des sessions futures.

**MODE D'EXÉCUTION (dev 2026-07-12, session de nuit autonome) :**
le dev ne peut pas tester cette nuit. TOUTES les étapes s'enchaînent :
1. **D'abord boucler le chantier 9** : C9.7 (save async + mesures) puis
   C9.8 (localtime_s portable, checklist Fedora, décision macOS/Vulkan
   inscrite dans MEADOWS-PLAN + CLAUDE.md §2.1).
2. Puis l'ouverture du chantier followers : `docs/CHANTIER-FOLLOWERS.md`
   (ce plan), CLAUDE.md **§2.11 « Reuse before build »** + rappel §10,
   ligne MEADOWS-PLAN, mémoire projet — un commit doc.
3. Puis **É0 → É10 à la suite**, un commit par étape (retour arrière
   possible), doctests + suite complète + smoke-run à chaque commit ;
   **É11 montures : v1 (chevaucher/descendre, vitesse, sifflet) SI le
   temps le permet**, feel à retoucher avec le dev.
4. **`docs/FOLLOWERS-TEST.md` tenu AU FIL DE L'EAU** : pour chaque étape
   committée, quoi tester en jeu, comment, et le comportement attendu —
   c'est le protocole de validation du dev à son retour.
Data de test actée : 2-3 placeholders au village (guerrier « cri de
guerre », guérisseuse « soin », mercenaire archer), dialogues en
français comme le contenu village existant — le dev ré-autorise après.

## LA CONTRAINTE STRUCTURANTE (demande dev, à graver)

> **Réutiliser avant de construire.** Chaque fonctionnalité follower
> doit s'appuyer sur un système existant nommé ; on n'écrit un nouveau
> système que si l'existant ne peut pas l'exprimer, et on l'écrit alors
> comme extension du pattern le plus proche. Toute brique du chantier
> CITE le système réutilisé dans son commit.

## Carte de réutilisation (vérifiée par 3 agents d'exploration)

**Déjà construit POUR les followers (chantier 5, cité par son journal
« le futur chantier followers n'ajoutera que le package IA follow + un
tag — zéro travail de persistance restant ») :**
- **Persistance** : référence `cell = 0` = persistante (le joueur l'est
  déjà) ; recruter = patcher `ReferenceForm.cell → 0` + retirer la
  relation `InCell` ; renvoyer = patcher `cell → home`. `captureReference`
  diffe déjà `cell` + `position` → la save porte recrutement/renvoi
  gratuitement ; un mort recharge mort (`applySavedState`).
- **Simulation par PNJ** : `tickCharacter` tourne DÉJÀ pour chaque PNJ
  (survie faim/soif/fatigue, blessures, résonance, régén, stagger…) —
  « appliquer la survie du joueur aux followers » est acquis au niveau
  sim ; seules les RÈGLES de conséquence manquent.
- **GAS** : pouvoirs = `AbilityForm` (les PNJ font déjà `tryActivate`) ;
  perks = effets infinite / abilities accordées ; âge = 2 effets
  infinite multiply < 1 ; courbes de classe = bases `CoreAttributes` ou
  le mécanisme `maxHealthOverride` (`override ?? formula`, efdf8e7).
- **Conditions** : l'évaluateur Phase 4 (`ConditionForm` : HasTag /
  AttributeAtLeast / HasItem / Lua, clauses AND par parent) gate DÉJÀ
  les options de dialogue — recrutement, déblocages d'affinité,
  dialogues de refus contextuels = ce système tel quel. Quêtes déjà
  miroir en tags `Quest.<Id>.Active/.Done`.
- **Dialogue** : options conditionnées + `event` (déclenche quêtes,
  actions de scène — précédents OpenBarter/OnPayFine) + `takeItem`
  (l'or de l'amende = le gabarit du paiement mercenaire/forge).
- **Équipement/économie** : `Inventory`/`Equipment` agnostiques à
  l'acteur ; l'écran conteneur ouvre l'inventaire de N'IMPORTE quelle
  entité avec équipement intégré (`toggleEquip`) = l'accès à
  l'équipement du follower tel quel ; helpers d'encombrement purs +
  `maxEncumbrance` dérivé pour tout acteur ; or = item ordinaire.
- **Timers en temps de jeu** : `GameClock` + le pattern `VendorState.
  lastRestockHours` (= anti-répétition 10 h, contrats 7 jours, 30 jours
  forge) ; effets `durationHours`.
- **UI** : gabarit écran = UiScreenForm + contrôleur (Options/Map C9.4/6),
  nameplates PNJ (= le party frame), pipeline loc C9.5, nav manette C9.3.
- **Aggro sur le bus** : `OnHitTaken{source, target}`, `CallForHelp`,
  `OnDeath` — le signal « on attaque le joueur » existe.
- **Tombe** : corpse→conteneur existe ; la couche pending sait déjà
  MATÉRIALISER un record (disableReference) — à généraliser en
  « creates » complet.

**Manques STRUCTURELS identifiés (à ne pas découvrir en cours de route) :**
1. **Aucun système de niveau/XP/compétences** (STATS.md le diffère ; le
   doc followers s'appuie partout sur le niveau). → É0 pose un niveau
   MINIMAL (attribut) ; la progression du joueur (skills-by-use) reste
   son propre chantier.
2. **Combat PNJ-contre-PNJ inexistant** : la RÉSOLUTION est agnostique
   (resolveMeleeStrike, R1) mais le contrôleur vise `ctx.player` en dur.
   → É2 généralise sur une cible-entité.
3. **Pas d'état « à terre »** : PV 0 = State.Dead direct. → É3, sur le
   pattern stagger/paralysie de `CombatState`.
4. **Pas de réputation** (design seulement) → mercenaires v1 sans; le
   gabarit est `Bounty` (score numérique miroir tag) le jour venu.
5. **Pas de menu radial** → commandes de groupe v1 = menu type « wait ».
6. **`grantedAbilities` non sauvegardé** → à persister (pattern child
   records, comme les quêtes) dès É6.
7. **Montures = tech nouvelle** (chevaucher un corps en mouvement) →
   dernière étape, découplée, possiblement son propre chantier.

## Les étapes (chaque étape = jouable + doctests + commit ; jalon dev 🎮)

### É0 — Socle (données + niveau minimal)
- `ActorForm` += (APPEND reflet) : `followerCategory` (str vide/major/
  minor/mount), `followerClass` (guid), `age` (f32 0=ignore), `minLevel`,
  `mainCharacter` (bool), `homeMarker` (guid), `recruitDialogue` (guid si
  distinct du dialogue normal), `buryMarker`+`buryContact` (É8).
- Nouveau `FollowerClassForm` (data/gameplay) : courbes niveau→9
  attributs (base + parNiveau), style de combat (str → CombatAi),
  perks par palier = children `ClassPerkForm {level, ability|effect}`
  (pattern childrenOf/LoadoutEntryForm).
- Nouveau composant réfléchi `gameplay::FollowerState` {active, level,
  affinity, hoursTogether, contractExpiryHours, lastLevelSyncedFrom,
  downState…} + lignes de capture Pattern A (`SavedStatsForm` append +
  `componentToSaved/savedToComponent`) + registration.
- **Niveau minimal** : attribut `level` (AttributeSet append, base=1) —
  le joueur en a un (posable console/data), les conditions
  `AttributeAtLeast level` marchent. AUCUN gain automatique ici.
- Doctests : classe→attributs à niveau N, save round-trip FollowerState.

### É1 — Le follower marche 🎮 (vertical minimal)
- Recruter/renvoyer par dialogue : option conditionnée (évaluateur) →
  `event` → handler scène qui PATCHE `cell` (pending layer, convention
  chantier 5) + retire `InCell` + set `FollowerState.active`.
- Package IA **`follow`** : branche `kind == "follow"` dans
  `NpcScheduleController` (goTo → position joueur, repath périodique,
  `speedScale ~1.25` au-delà d'un seuil — le knob existe) ; en deçà
  d'un rayon, idle/regarde le joueur. Champs de feel = StatsTuningForm.
- Réapparition : après `performTravel` et si distance > seuil,
  reposition près du joueur (grounded `terrain::height`), path clear.
- Data : UN follower de test au village (ActorForm + dialogue de
  recrutement + home marker).
- Doctests : la décision follow (fonction pure position→intention),
  patch cell recruter/renvoyer via pending layer.
- 🎮 Recruter, être suivi (y c. à travers une porte), renvoyer, F5/F9.

### É2 — Il se bat à tes côtés 🎮
- **Généraliser la cible** : `Npc.combatTarget` (entité) ; le
  contrôleur combat lit la cible (position/Transform, StatBlocks,
  capsule via `segmentHitsActor`) au lieu de `ctx.player` — la
  résolution R1 est déjà prête. Le PNJ hostile garde le joueur pour
  cible par défaut (comportement inchangé).
- Aggro follower : subscribe `OnHitTaken` (target == joueur OU un
  follower actif) → le follower adopte `event.source` ; `OnDeath` de la
  cible → désengage. Followers dans le camp joueur pour la perception
  des hostiles (les bandits peuvent cibler un follower : même
  mécanisme, cible = plus proche via SpatialIndex).
- Flag « épreuve amicale » (les followers restent passifs) : tag.
- Party frame HUD : rows nameplates filtrées followers (pattern C9.x).
- Doctests : adoption/désengagement de cible (bus headless), le choix
  de cible.
- 🎮 Duel au camp : le follower engage l'agresseur, les bandits se
  répartissent.

### É3 — À terre, soin, survie, rotation 🎮
- `State.Downed` : timer `downedSeconds` sur `CombatState` (pattern
  stagger) ; pour un follower actif, 0 PV → Downed au lieu de Dead
  (gate au point unique `updateLifeState`). Anim à genoux = gate anim
  tag (pattern sitting/dead).
- Relever : interaction contextuelle (PromptKind existant + objet de
  soin consommé, restauration partielle) ; « E to heal ».
- Plancher de survie follower (pas de mort par conditions extrêmes) ;
  aggravation : blessure sur déjà-blessé → proba (RNG seedé §8) de
  blessure grave / mort réelle.
- Convalescence : follower blessé exige le repos → renvoi automatique
  vers home (le schedule existant), indisponible tant que
  `Injury.recoveryHoursRemaining` court ; dialogue de consultation
  (état/blessures/timers — modèle RmlUi type recruit-preview É4).
- Doctests : routage Downed vs Dead, plancher survie, proba
  d'aggravation (RNG seedé), indisponibilité.
- 🎮 Le follower tombe, tu le relèves ; blessé deux fois il rentre se
  soigner — tu tournes sur un second follower.

### É4 — Recrutement complet + affinité 🎮
- Conditions de recrutement en DATA : clauses `ConditionForm` sur les
  options du dialogue de recrutement (niveau, attributs, HasItem or,
  tags de quête/faction/équipement) — l'évaluateur tel quel ; refus
  contextuels = options sœurs à conditions inversées (negate) portant
  l'indice.
- Affinité : `FollowerState.affinity` ; croissance passive par
  gameHours ensemble (pattern VendorState) ; ± par actions (events du
  bus filtrés par le handler follower, configurables par children
  `AffinityRuleForm {event, filterTag, delta}`).
- Nouvelle clause d'évaluateur `FollowerAffinityAtLeast` (une entrée de
  table de dispatch — le point d'extension OCP documenté).
- Aperçu des stats au recrutement : écran RmlUi (gabarit contrôleur
  C9.4/C9.6, `currentValueOf` sur l'entité cible, loc C9.5).
- Doctests : refus contextuel (bonne clause échouée → bonne option),
  croissance/deltas d'affinité, la clause nouvelle.
- 🎮 Un mercenaire refuse tant que < niveau/or ; l'affinité débloque un
  dialogue.

### É5 — Classes, niveaux, évolution
- Application des courbes : au changement de `FollowerState.level`,
  écrire les bases `CoreAttributes` depuis la classe (+ éventuels
  overrides) puis `initializeActorStats`-recompute (mécanisme efdf8e7).
- Âge : 2 effets infinite multiply (<1) posés au spawn depuis
  `ActorForm.age` (courbe âge→multiplicateurs en tuning data).
- Leveling lié au joueur : actif → suit le niveau joueur ; inactif →
  au re-recrutement, rattrape la moitié de l'écart ; `mainCharacter` →
  rattrape tout. (Le niveau JOUEUR ne monte pas encore tout seul —
  chantier skills ; posable console pour tester.)
- +1 compétence à la montée du joueur : v1 sur les ATTRIBUTS (l'algo du
  doc, décroissant, premier attribut joueur > follower) — les skills
  n'existant pas encore, noté comme tel.
- Doctests : courbes, rattrapage ×0.5 / mainCharacter, algo du +1.

### É6 — Pouvoir spécial + perks réciproques 🎮
- Pouvoir = `AbilityForm` accordée au spawn (grantAbility) ; l'IA
  follower l'active en combat (`tryActivate` — précédent NPC) selon son
  style de classe.
- **Persister `grantedAbilities`** (manque identifié) : child records
  save (pattern SavedQuestForm).
- Évolutions du pouvoir + perks appris par le joueur : children
  `PowerEvolutionForm` / `TaughtPerkForm` {conditions (évaluateur),
  dialogue spécial, ability/effect accordé} ; contrainte de lieu =
  HasTag de zone (tags de trigger volumes existants) avec notification
  si lieu inadapté.
- 🎮 Le follower claque son pouvoir ; un dialogue t'apprend une perk au
  coin du feu.

### É7 — Équipement du follower 🎮
- Accès : action `InteractAlt` (ActionMap C9.2, chord pad LB+X p.ex.) →
  `openContainerScreen(follower)` TEL QUEL (transfert + équipement
  intégré) ; gate d'affinité négative (dialogue de refus).
- Flag `unremovable` (APPEND sur les 4 item forms) gardé dans
  transferItem/toggleEquip ; encombrement par follower (helpers
  existants × multiplicateur d'âge, rejet à l'ajout) ;
  `applyEquipmentModifiers` appelé aussi dans le tick PNJ (manque
  identifié : l'armure ne protège pas les PNJ aujourd'hui).
- Auto-équipement : à la réception d'un item, comparaison via la
  colonne `power` d'InventoryView (mieux → équipe, dialogue poli).
- Forge : paliers d'obsolescence (children data), dialogue gated
  (ville avec tag forge) + coût or (pattern payFine/takeItem), pathing
  vers le POI forge (goTo existant) ; 30 jours hors groupe = timestamp
  gameHours (pattern VendorState) ; mercenaires sans coût.
- Doctests : unremovable, rejet de surpoids, comparaison d'items,
  déclenchement du palier.

### É8 — Mort, tombe, enterrement 🎮
- Mort réelle (conditions É3) → **création runtime d'une référence
  persistante** : généraliser la matérialisation de `PendingSaveLayer`
  (disableReference) en « creates » complet {guid déterministe
  Guid::combine, base = GraveForm conteneur, cell=0, position} +
  `spawner.spawn` live. L'inventaire du mort y est transféré.
- Interactions tombe : normale = hommage (anim gate + cue) ; spéciale
  (InteractAlt) = conteneur (dépôt d'objets/fleurs = le transfert
  existant, dans les deux sens).
- Les 3 enterrements : sur place ; cadavre = item porté (flag) puis
  enterrer à l'endroit choisi ; PNJ proche = tombe au `buryMarker`
  autoré.
- Doctests : matérialisation du record de tombe + round-trip save.
- 🎮 Un follower meurt pour de vrai ; sa tombe survit au F9 ; tu y
  déposes une fleur.

### É9 — Multi-followers, commandes, vie ambiante
- Cap 5 majeurs + 6 mineurs (catégorie ActorForm ; contrôle au
  recrutement) ; party frame étendu.
- Affinité inter-followers : matrice en child records (Pattern B) ;
  dialogues inter-followers (déclencheur événement commun / timer,
  paires avec contenu).
- Commandes de groupe v1 : menu type « wait » (suivre/rester/attaquer/
  défendre) — le radial reste net-new, différé.
- Interactions ambiantes : commentaires par lieu/type de lieu (tags) et
  événement, PAS en sneak ; anti-répétition (timestamp par interaction,
  défaut 10 h data, flag one-shot) ; chaînage ordonné (prérequis +
  cooldown min) — tout en children data + FollowerState.
- Soin inter-followers en combat (IA : allié à terre + objet de soin,
  réserve pour le joueur) ; fin de combat étendue.
- Sandboxing en ville (schedules contextuels — le système existant).

### É10 — Mercenaires
- Contrat : `contractExpiryHours` (gameHours), notification approche/
  expiration (toast loc), renouvellement par dialogue payant (takeItem
  or) ; prix v1 = f(niveau joueur, richesse via itemCount or) — la
  réputation viendra avec son propre système (gabarit Bounty).

### É11 — Montures (découplée, possiblement chantier propre)
- Tech nouvelle : chevaucher (attache caméra/corps à un PNJ en
  mouvement, vitesses), sifflet, écurie/monture principale, stats
  montures (poids/vitesse/courage), fuite/désarçonnement/retour,
  immortalité des montures de followers, cueillette montée. À
  re-planifier en briques quand É1-É10 tiennent — RIEN dans É0-É10 ne
  doit s'y coupler (les montures sont `followerCategory = mount`, hors
  cap, dès É0 pour que la donnée soit prête).

## Ordre, jalons, garde-fous

É0 → É1 🎮 → É2 🎮 → É3 🎮 → É4 🎮 → É5 → É6 🎮 → É7 🎮 → É8 🎮 → É9 →
É10 → (É11). Chaque étape : build + suite headless complète + smoke-run
greps durcis + commit par brique ; jalon dev en jeu aux 🎮.
- **Réutiliser avant de construire** (la contrainte §2.11) : chaque
  commit cite le système réutilisé.
- Reflets APPEND only ; §2.9 (l'affinité N'EST PAS un attribut GAS —
  composant réfléchi dédié, elle ne passe pas par applyEffect) ; §2.10
  headless (toute la logique follower est sim, doctestable) ; RNG seedé
  §8 pour l'aggravation ; feel en Forms/StatsTuning, jamais en dur.
- Le combat PNJ-contre-PNJ (É2) est LE risque technique : le faire tôt,
  iso-comportement pour les hostiles actuels (cible par défaut = joueur).

## Texte de la contrainte pour CLAUDE.md (à l'exécution)

§2 (après 2.10) :
> 11. **Reuse before build.** Before writing any new system, name the
>     existing one that covers the need (GAS effects/abilities, the
>     condition evaluator, dialogue events, child-record data patterns,
>     the pending save layer, schedules/AI packages, game-clock
>     timestamps, the container/barter UI, RmlUi screen controllers…).
>     New code is an EXTENSION of the nearest existing pattern, never a
>     parallel mechanism. If nothing fits, say so explicitly and ask
>     before inventing. (Dev directive 2026-07-12, FOLLOWERS planning.)

§10 : ajouter le rappel « If a new feature seems to need a new system,
re-read §2.11 first ».

## Vérification globale du chantier

- Par étape : doctests sim (la logique follower est headless par
  construction) + smoke + jalon dev.
- Fil rouge de validation : un « tour complet » — recruter, voyager,
  combattre, tomber/relever, blesser/rotation, s'équiper, apprendre une
  perk, mourir/tombe — rejouable après F5/F9 à chaque étape.
- Le compte de forms/loc au boot est le témoin data (nouveaux Forms =
  attendus et notés par brique).
