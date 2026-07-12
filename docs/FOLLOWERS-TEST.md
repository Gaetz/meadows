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

<!-- Les sections d'étapes s'ajoutent ci-dessous au fur et à mesure. -->
