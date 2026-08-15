# Audit de conformité — docs/STATS.md vs implémentation (2026-08-15)

Audit règle par règle du design des stats contre `gameplay/stats`,
`gameplay/ability` et le câblage 3D. Légende : **C** conforme ·
**D** déviation · **M** manquante · **NB** non branchée (code présent,
aucun appelant 3D) · **F** marquée future dans le doc lui-même.

État après les correctifs du 2026-08-15 (voir « déjà corrigé » plus bas).

## Verdict d'ensemble

Le socle est très conforme : les 9 attributs et leurs maxima (§1), la
résonance et la cascade d'harmonie (§2, exemple du doc reproduit
exactement), la quasi-totalité des formules dérivées défensives (§3), les
buildups élémentaires, le modèle de combat §4 (coûts d'énergie,
exhaustion, sneak, parade/perfect parry/critical weakness, fall damage) et
les tables de blessures §5 (malus par partie/sévérité, 24/48/72 h par
rang, aggravation, récupération au repos) sont implémentés fidèlement,
avec les constantes de `StatsTuningForm` alignées sur le doc.

## Déjà corrigé (2026-08-15)

1. **Buildups d'arme jamais appliqués en 3D** (PoisonDagger/BleedMace/
   FlameScimitar inertes) → `DamageEvent` porte désormais
   `buildupType/buildupAmount` (remplis par `weaponDamageEvent`, scalés
   par `scaledStatusDamage` — ce qui branche aussi la formule « status
   damage » §3 qui n'avait aucun appelant) ; appliqués dans
   `resolveStrikeDamage` sur les coups passés (un blocage total ne laisse
   pas de résidu). Test doctest ajouté.
2. **Consommables à buildup no-op** : `UiRouter::useItem` appelait
   `applyEffect` sans le pointeur `StatusBuildup` → toute la famille
   antidote/fiole était silencieusement ignorée. Corrigé.
3. (Avant l'audit : `rollInjury` re-câblé dans la frappe ; sommeil/attente
   routés par `advanceGameTime` ; guérison gated sur le repos.)

## Décisions design à trancher (déviations / manquants non-futurs)

| # | Règle du doc | État du code | Impact |
|---|---|---|---|
| 1 | 0 PV ⇒ inconscient, mort après `constitution` heures, achèvement / stabilisation (1 PV après `max(1, 10−con/2)` min) | Mort immédiate ; `Downed` réservé aux followers protégés (30 s fixes, jet 35 %) | Élevé — boucle mort/soin entière |
| 2 | Les attributs montent via les seuils de skills (+ level-up +5 max) | `applyEffect` ne sait pas écrire les bases de `CoreAttributes` (no-op silencieux) ; aucune data de skills ; aucun code de level-up | Élevé — progression impossible |
| 3 | Attributs authorables par acteur/origine | `ActorForm` sans champs d'attributs → tous les PNJ à 6/6/6, insight 0 | Élevé — pas de diversité PNJ |
| 4 | `energy regen = 35 + alacrity`/s | `energyRegenBase = 17.5` | Élevé — cadence de combat ÷2 (tuning volontaire ? doc à mettre à jour sinon) |
| 5 | Reprise de regen après `0.9 − alacrity·0.1` s (min 0.5) | Constante 1.0 s non scalée (`kEnergyRegenDelay`) | Moyen |
| 6 | `shaken` = stagger court annulable au dodge | Tag posé/retiré mais aucun consommateur (décoratif) | Moyen |
| 7 | `critical damage = 1.5 + (dexterity − 0.5)` | `1.5 + dex·0.05` (commentaire : « surely meant per-point ») | Moyen — trancher, écrire au doc |
| 8 | `posture damage = base + base·(strength−5)%` | `base × scaling générique de l'arme` | Moyen |
| 9 | Maladies/psychoses infligées en jeu | `inflictEffect` prêt, aucune source ni data | Moyen — décider les sources |
| 10 | Résonance bornée −100..1500 | Aucun clamp | Moyen |
| 11 | Tags `Status.Wound/Exhaustion/Stress/Stimulation` (état de résonance) | Inexistants | Moyen — pas de hook UI/IA/dialogue |
| 12 | Fracture : max deux simultanées | Aucun plafond | Faible-moyen |
| 13 | `attack speed`, `bonus attack`, `range` (stat), `dodge` (stat), motion values | Absents | Faible-moyen |
| 14 | Dégâts non-crit réduits pendant la prostration | Absent | Faible |
| 15 | `essence regen = 0.005·insight` | `0.01 + 0.0025·insight` | Faible |
| 16 | `maxPosture` lit la base d'alacrité (doc : secondaires en current) | Base | Faible |
| 17 | Overencumbered « no jump/sprint/mount » | Un seul point de blocage vérifié | Faible |
| 18 | Partie du corps des blessures | Figée Torso (tables par partie prêtes dans Injuries.cpp) | Faible |
| 19 | `stealth speed = 80 + ala·2 + dex + gra` | Facteur forfaitaire 0.75 | Faible |

## Le DOC est en retard sur le code (à documenter dans STATS.md)

- Survie : `survivalThreshold 75`, `survivalResonanceAtEmpty −50`,
  décroissances 0.96/0.32/0.72 h/pt, `comfortableSleepHours 8`,
  `sleepPerHour 2` — cohérents entre eux, absents du doc.
- Electrocution coupe aussi `essenceRegen ×0` (ajout non documenté).
- `acceleration = 90 + alacrity·2` (formule inventée, saine, à écrire).
- La pénétration n'amplifie jamais une vulnérabilité (extension saine).
- Chance de blessure par fraction de PV retirée (bruise >5 %, cut >10 %,
  fracture >50 %) — approximation assumée des tables `[7+]`.
- Résistances sonic/chemical/psychic/holy/dark/ether déjà en formule
  (le doc les marque `[8]`).

## Nettoyage résiduel

- `AttributeSet.armorRating` : aucun lecteur (vestige 2D).
- `StatsTuningForm.bleedBurstDamage` : marqué OBSOLETE dans le code.
- `registerStatsComponents` : appelé uniquement par les tests — les
  composants stats du jeu 3D passent par le Spawner mais ne sont pas
  enregistrés dans le pont réflexion du World (à vérifier vis-à-vis de la
  sérialisation ; `SaveState` gère Injuries/Resonance explicitement).

## Marquées FUTURES dans le doc (pas des manquements)

Température→onyx `[7]` ; résonance par usage des stats et barres qui ne se
remplissent plus `[7]` ; +0.001/pt sur repos 8 h `[7]` ; « 8 h retirent
10 points » (drogues) `[7]` ; stats sociales `[7]` ; stealth/climb/swim/
breath `[7]` ; démembrement `[7]` ; sous-états de coupure, objets de
traitement, tables de sources `[7+]`.
