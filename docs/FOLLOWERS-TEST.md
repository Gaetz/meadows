# FOLLOWERS — protocole de test dev

> Tenu au fil de l'eau pendant la session de nuit du 2026-07-12. Chaque
> étape committée ajoute sa section : quoi tester, comment, comportement
> attendu. Les étapes sont committées séparément — `git log --oneline`
> te donne les points de retour arrière. Plan : `docs/CHANTIER-FOLLOWERS.md`.

## Avant tout

- Build CLion habituel ; le boot doit afficher `Plugin stack` /
  `Loc: … strings` sans warning, et les timings `Load:` (C9.7).
- Les placeholders de test vivent au village : **Aldric** (guerrier,
  cri de guerre), **Maela** (guérisseuse, soin), **Corvin** (mercenaire
  archer). Renomme/réécris-les librement — tout est data (village.toml).
- Ta save existante doit continuer de charger à chaque étape (reflets
  APPEND only). Si une étape casse ta save, c'est un bug à me remonter.

## É0 — Socle data (rien de visible en jeu)

Étape purement data-model : champs follower sur ActorForm,
FollowerClassForm/ClassPerkForm (courbes de classe), composant
FollowerState (préfixé follower* dans la save), attribut `level` (base
1) sur tout acteur.

**À tester :**
1. Ta save existante charge sans erreur ni régression (F9) — c'est LE
   test de l'étape (reflets append-only).
2. Boot : toujours `7 plugins, 412 forms`, zéro warning.

## É1 — Le follower marche (Aldric)

**Aldric** est sur la place du village (~x44 z367), tenue de villageois.

**À tester :**
1. Parle-lui [E] → « Voyagerons-nous ensemble ? » → il te suit :
   marche quand tu marches, ACCÉLÈRE (+25 %) quand tu le sèmes, te
   regarde quand tu t'arrêtes à côté.
2. Sème-le franchement (> ~40 m ou sprint prolongé) → il se TÉLÉPORTE
   près de toi (posé au sol, pas dans un mur).
3. Passe une porte (intérieur/extérieur) → il réapparaît près de toi
   à l'arrivée.
4. F5 puis F9 pendant qu'il te suit → il est toujours là, toujours
   actif (le contrat cell=0 du chantier 5 porte tout).
5. Re-parle-lui → « Reste ici pour l'instant. » → il rentre vers son
   marker de la place (son schedule le garde là) ; F5/F9 → il est chez
   lui, plus actif.
6. Vérifie qu'il ne se DUPLIQUE jamais (recruté puis retour au village
   d'origine — le garde anti-double est doctesté mais l'œil confirme).

**Comportement attendu ailleurs :** les 4 autres PNJ inchangés (les
bandits attaquent, le garde/villageois vaquent).

## É2 — Il se bat à tes côtés

**À tester (avec Aldric recruté) :**
1. Va au camp est ; laisse un bandit t'attaquer → au premier coup reçu
   (ou dès que TU frappes un bandit), Aldric ENGAGE l'agresseur : il
   court dessus, dégaine et échange des coups (logs « É2: Aldric
   engages Bandit » puis « Aldric's blade lands on Bandit »).
2. Le bandit frappé par Aldric se retourne CONTRE lui (il ne reste pas
   fixé sur toi) ; l'archer qui te touche d'une flèche déclenche aussi
   la défense.
3. Le **party frame** en haut à gauche montre « Aldric » + sa barre de
   vie qui bouge pendant l'échange.
4. Mort de la cible → Aldric désengage et revient te suivre.
5. Hors combat : rien ne change (villageois/garde inertes, pas
   d'aggro fantôme) ; les bandits SANS follower se comportent comme
   avant (régression : leurs lignes B3 habituelles).
6. F5/F9 en plein combat → au reload, plus de cible (elle se ré-acquiert
   au prochain coup) — voulu, la cible n'est pas persistée.

**Réserves connues :** Aldric ne dodge pas (les PNJ n'esquivent pas
encore) ; pas d'IA d'archer follower ; le tag `Combat.FriendlyTrial`
sur le joueur coupe l'adoption (pour les futures épreuves amicales).

## É3 — À terre, soin, survie, rotation

Tu démarres avec **2 Potions de soin** dans l'inventaire (loadout).

**À tester (Aldric recruté, combat au camp) :**
1. Laisse Aldric encaisser jusqu'à 0 PV → il tombe **à terre**
   (posture assise v1 — pas d'anim agenouillé en stock), sort du
   combat, les ennemis le LÂCHENT et se retournent vers toi.
2. Approche-toi → prompt « [E] Soigner Aldric (potion) » → consomme
   une potion, il se relève avec un partiel de vie et te resuit. Sans
   potion : toast d'excuse.
3. Ne le soigne PAS (~30 s à terre) → tirage seedé : soit il MEURT
   pour de vrai (mort normale : loot, cadavre), soit il se relève à
   1 PV avec une **blessure** (torse). Blessé une deuxième fois : le
   tirage d'aggravation peut empirer la blessure ou le tuer.
4. Blessé → il exige le repos : il rentre CHEZ LUI tout seul
   (auto-renvoi) et refuse le re-recrutement (« Follower.Convalescent »
   — l'option de dialogue le dit) tant que la guérison court.
5. « Comment te sens-tu ? » dans son dialogue → toast avec vie %,
   blessures, heures de repos restantes.
6. La survie ne peut PAS le tuer (elle ne touche que la résonance —
   prouvé par doctest, rien à voir de spécial en jeu).
7. F5/F9 pendant qu'il est à terre → il RECHARGE à terre, chrono
   intact.

**Réserves :** visuel « à terre » = pose assise (l'anim viendra) ; le
miroir Convalescent est exact quand sa cellule est résidente (le cas
dès que tu peux lui parler).

## É4 — Recrutement conditionnel + affinité (Maela)

**Maela** (guérisseuse) est près du coin de la place d'Aldric.

**À tester :**
1. Parle-lui direct → REFUS avec indice : « Nous nous connaissons à
   peine… » (affinité insuffisante). Chaque salut lui donne +5
   d'affinité : au 2e passage l'affinité atteint 10 → le refus change :
   « Tu manques d'expérience pour la route. » (niveau 2 requis — ton
   niveau est 1, l'attribut `level` d'É0 ; console pour le poser à 2 en
   attendant le chantier progression).
2. Niveau ≥ 2 + affinité ≥ 10 → l'option de recrutement apparaît.
3. « Parle-moi de tes aptitudes » (elle ET Aldric) → l'écran
   d'aperçu : classe, niveau, les 9 attributs, vitals, affinité.
4. Affinité par actions : une parade parfaite d'un ennemi devant
   Aldric = +5 ; le FRAPPER toi-même = −10 (logs « affinity » au
   passage).
5. Le temps ensemble compte : +0,5/heure de jeu avec un follower actif
   (attends au menu T pour le voir grimper via l'aperçu).
6. F5/F9 : l'affinité et les heures survivent.

## É5 — Classes, niveaux, évolution

Le niveau JOUEUR ne monte pas encore tout seul (chantier progression) :
la console (F8) a maintenant **`player.level <n>`** pour tester.

**À tester :**
1. `player.level 3` → recrute Aldric → log « É5: Aldric level 1 -> 3
   (+1 strength) … » : il suit ton niveau, ses attributs montent selon
   sa classe (guerrier : force/constitution), et chaque niveau gagné
   ACTIF lui donne +1 dans ta meilleure stat supérieure à la sienne.
2. Renvoie-le, `player.level 6`, re-recrute → il rattrape la MOITIÉ de
   l'écart (l'exception mainCharacter — rattrapage complet — est un
   flag data, aucun follower de test ne l'a).
3. L'aperçu de Maela (52 ans) montre ses attributs COURANTS réduits par
   l'âge (~-5 % physique / -3,5 % mental) ; Aldric (28 ans) : aucun
   effet (seuil 45 ans).
4. Un level-up ne SOIGNE pas : la vie courante est préservée, seuls
   les maxima/stats dérivées bougent.
5. F5/F9 : niveaux (joueur et follower) et bonus accumulés survivent.

**Note design à trancher :** les followers gardent leur maxHealth
autoré (60/45) qui ÉPINGLE leur vie max (le mécanisme override) — le
niveau fait monter attaque/défense/posture mais PAS la vie max.
Retirer `maxHealth` de leur ActorForm les fait passer à la formule
d'attributs (~100 au niveau 1). Une ligne de data, à ton choix.

## É6 — Pouvoirs spéciaux + perks réciproques

**À tester :**
1. Recrute Aldric, engage un combat → il déclenche son **Cri de
   guerre** (log « É6: Aldric unleashes his power ») : buff +force
   +régén posture 20 s, cooldown 45 s (coût énergie — les mêmes règles
   GAS que toi).
2. Recrute Maela, laisse ta vie passer sous 50 % en combat → elle te
   **soigne** à distance (+30 vie, coût essence, cooldown 12 s) ; elle
   soigne aussi Aldric et se tient à ~8 m du danger (style healer).
3. **Apprends une perk** : affinité Aldric ≥ 25, place du village (la
   zone calme — un volume trigger, extensible en data) → option
   « Apprends-moi quelque chose. » → tu gagnes « Second souffle »
   (+5 régén d'énergie, permanent, tag Perk.SecondSouffle). Hors de la
   zone : la réplique t'indique de trouver un endroit tranquille.
4. F5/F9 : les abilities accordées (pouvoirs ET perks) survivent —
   c'était un trou identifié (grantedAbilities n'était pas sauvegardé).
5. Une perk ne s'apprend qu'UNE fois (le tag dédoublonne).

## É7 — Équipement du follower

Nouvelle action **InteractAlt** : **F** au clavier, **LB** à la manette
(le Shift+E du doc demanderait des chords — extension possible plus
tard ; remappable dans les options comme le reste).

**À tester :**
1. Vise Aldric → le prompt affiche aussi « [F] Équipement » → **F**
   ouvre son inventaire/équipement (l'écran conteneur habituel :
   transfert + équiper). Son ÉPÉE DE BASE (« Arme d'Aldric »,
   non-retirable) ne peut pas lui être prise ; s'il t'en veut
   (affinité < 0, frappe-le plusieurs fois), il REFUSE l'accès.
2. Donne-lui une meilleure arme/armure → il s'équipe TOUT SEUL avec un
   toast poli ; reprends-la → il retombe sur son épée de base.
3. Surcharge : impossible de lui transférer au-delà de son poids max
   (stat × facteur d'âge — Maela porte un peu moins).
4. L'armure PROTÈGE désormais les PNJ (donne un plastron à Aldric et
   compare les dégâts qu'il encaisse).
5. **Forge** : près de l'établi du village (zone), avec ≥ 50 or,
   l'option « Améliorer ton équipement (50 or) » apparaît dans son
   dialogue → son épée passe au palier supérieur (« Arme d'Aldric+ »),
   l'or est débité ; rien à améliorer = pas de débit. Hors zone : la
   réplique t'oriente vers la forge.
6. F5/F9 : équipement, épée de palier et refus persistent.

**Réserves v1 (notées en TODO data) :** palier d'obsolescence par
niveau, le follower qui MARCHE à la forge, l'auto-amélioration à 30
jours, la gratuité mercenaire (É10).

## É8 — Mort, tombe, enterrement

Tu démarres avec **2 Fleurs** (loadout). La tombe utilise le rocher
moussu en placeholder de stèle (l'asset viendra).

**À tester (il faut une VRAIE mort — laisse Aldric à terre sans potion
et perds le tirage, ou recommence) :**
1. Sur son cadavre : [E] = fouiller (comme avant) ; **[F] = « Enterrer
   ici »** → une tombe apparaît sur place, TOUT son inventaire y est
   transféré, le cadavre disparaît.
2. Sur la tombe : **[E] = hommage** (toast avec son nom + cue) ;
   **[F] = le conteneur** → dépose une Fleur (et reprends-la si tu
   veux — le transfert marche dans les deux sens).
3. **Par le contact** : au lieu d'enterrer toi-même, parle au
   Villageois (le contact d'Aldric) → « Un compagnon du village est
   tombé... » → la tombe apparaît au petit cimetière du village
   ([38, 377]), inventaire transféré, cadavre retiré même si sa
   cellule n'est pas chargée. Sans mort à enterrer : « Personne à
   enterrer ».
4. **F5/F9 : la tombe SURVIT** (référence persistante créée au
   runtime — le mécanisme de la couche pending généralisé), avec son
   contenu et les fleurs déposées.

**Différé (TODO noté) :** porter la dépouille pour l'enterrer à
l'endroit de ton choix — demande une mécanique de placement au sol qui
n'existe pas encore.

## É9 — Multi-followers, consignes, vie ambiante

**À tester (Aldric ET Maela recrutés — monte l'affinité de Maela en lui
parlant 2×, niveau 2 via `player.level 2`) :**
1. Les DEUX te suivent, le party frame liste les deux ; caps : 5
   majeurs / 6 mineurs (les placeholders n'y touchent pas — le refus
   « groupe complet » est doctesté).
2. **« Consignes de groupe… »** dans le dialogue de chaque follower
   (visible seulement s'il est actif) : Tous suivre / rester /
   attaquer ma cible / me défendre.
   - « Rester » : ils tiennent leur position (même à travers un
     voyage) ; « Suivre » reprend.
   - « Attaquer ma cible » : ils adoptent le DERNIER hostile que tu as
     frappé ; sans cible : toast dédié.
   - « Me défendre » : ils n'engagent PLUS sur ta simple initiative
     (frapper un bandit ne les lance pas), mais ripostent toujours si
     toi ou eux êtes touchés.
3. **Banter** : les deux proches (< 8 m), hors combat/sneak, toutes
   les ~2 h de jeu (menu T pour avancer) → un échange en deux toasts
   (« Aldric: … » puis Maela 3 s après). L'une des deux répliques est
   one-shot et gated par leur lien (BondAldricMaela 15).
4. **Commentaires ambiants** : Aldric commente l'arrivée sur la place
   (zone calme) et la mort d'un bandit (« Un de moins. ») ; le
   commentaire de Maela est CHAÎNÉ (il ne vient qu'après celui
   d'Aldric, ≥ 1 h plus tard, one-shot). Anti-répétition 10 h par
   commentaire ; jamais en sneak.

**V1 assumés :** menu radial différé (les consignes vivent dans le
dialogue) ; horloges anti-répétition non persistées (reset au load) ;
lien inter-followers = valeur autorée (pas encore de dynamique) ;
« rester » = l'état sandbox v1 (la vraie vie en ville groupée viendra).

<!-- Les sections d'étapes s'ajoutent ci-dessous au fur et à mesure. -->
