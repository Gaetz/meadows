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

<!-- Les sections d'étapes s'ajoutent ci-dessous au fur et à mesure. -->
