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

<!-- Les sections d'étapes s'ajoutent ci-dessous au fur et à mesure. -->
