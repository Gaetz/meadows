# Boss behaviors in Lua — design + v1 implementation

> Status : **briques 1-2 IMPLÉMENTÉES (2026-07-11, décision dev : « pour
> tous les types d'adversaires, bandit en exemple »)** — événements de
> combat sur le bus + `ActorForm.brainScript` avec tick de décision et
> retours de MOVES. Les briques 3-4 (API d'actions, boss de démo)
> restent à ouvrir.
>
> **Ce qui marche aujourd'hui** : tout ActorForm hostile peut porter un
> `brainScript` (source Lua qui RETOURNE `function(situation) ->
> "approach"|"strike"|"strafe"|"flee"`). Le director l'appelle à ~4 Hz
> (jamais par frame), compile UNE fois par form (toutes les instances
> partagent), exécute le move retourné ; erreur de compilation ou
> d'exécution → log UNE fois puis repli définitif sur le cerveau C++
> (`chooseCombatMove`). `self` est lié (hasTag/applyEffect/attributs en
> lecture) ; la table situation : {distance, attackRange,
> preferredRange, canSee, swinging, cooldown, healthFraction, courage,
> aware}. Les événements `OnHitTaken`/`OnStagger`/`OnParried` partent
> sur le bus depuis les deux chemins de dégâts. **L'exemple vivant : le
> Bandit de village.toml** — même réflexes que le C++ mais il craque à
> 40 % de santé ; supprimer le champ le rend au C++ pur.

## Le problème, et l'intuition du dev

L'IA actuelle est C++ par frame (perception → `chooseCombatMove` →
exécution NpcDirector) — c'est voulu et ça doit le rester : du Lua par
frame et par PNJ contredirait §2.8 (un seul VM, scripts stateless, pas
d'environnement Lua par entité) et le budget. L'intuition du dev est la
bonne : **sortir le script de la boucle chaude, le brancher sur des
ÉVÉNEMENTS** — le C++ garde les réflexes, le Lua prend les DÉCISIONS
rares.

## Ce qui existe déjà et sur quoi s'appuyer

- **UN VM partagé** (sol2), scripts stateless, `self` = handle d'entité,
  `wait()` latent via le pool de coroutines central (Phase 4) — le
  séquencement d'un pattern de boss (télégraphe → wait(0.8) → frappe)
  est DÉJÀ payé.
- **EventBus** à vocabulaire ouvert — OnDeath, OnNoise, CallForHelp,
  AnimEvent y transitent déjà; les quêtes s'y abonnent en data (8.7c).
- **GAS** : les attaques spéciales d'un boss sont des AbilityForms
  (coût/cooldown/effets/script d'ability) — de la DATA, pas du moteur.
- **`chooseCombatMove`** : LA couture de décision (une fonction, un
  enum) — le point d'insertion propre d'un « cerveau » alternatif.
- **ScriptVars** (composant reflété, §2.8) : l'état persistant d'un
  combat de boss (phase, compteurs) traverse la save gratuitement.
- **Perception** (composant reflété) : l'aggro/la mémoire sont déjà là.

## Design proposé (à valider)

**Un boss = ActorForm + un jeu d'AbilityForms + un script-cerveau +
des tags de phase.** Le C++ exécute, le Lua décide — rarement.

1. **Événements de combat sur le bus** (émis par NpcDirector, C++ pur,
   utile même sans Lua) : `OnAlert`, `OnLostSight`, `OnHitTaken`
   (dégâts, attaquant), `OnStagger`, `OnParried`, `OnSwingLanded`,
   `OnHealthThreshold` (seuils déclarés en data : 0.75/0.5/0.25…),
   `OnAllyDown`.
2. **`ActorForm.brainScript`** (guid, append) : présent → le director
   appelle `decide(self, situation)` à cadence BASSE (tick de décision
   ~4 Hz **+ sur événement**, jamais par frame). La `situation` est la
   `CombatSituation` exposée en table Lua (distance, canSee, cooldown,
   healthFraction…). Retour : soit un move standard (`"approach"`,
   `"strike"`, `"strafe"`, `"flee"` — le C++ l'exécute tel quel), soit
   une ACTION scriptée. Entre deux ticks, la dernière décision tient.
3. **API d'actions curée** (bindings sol2, tous passant par les chemins
   sanctionnés — jamais d'écriture d'attribut directe, §2.9) :
   `self:activateAbility(id)`, `self:applyEffect(id)`,
   `self:moveTo(pos)`, `self:faceTarget()`, `self:setPhaseTag(tag)`,
   `self:say(line)`, `spawnReference(form, pos)` (les adds) ; requêtes :
   `self:health()`, `target:position()`, `self:hasTag(t)`,
   `distanceToTarget()`. Les séquences latentes utilisent le `wait()`
   existant.
4. **Phases = GameplayTags** (`Phase.Enraged`…) : gates d'abilities,
   de transitions d'anim graph et de conditions — le vocabulaire §6
   déjà en place.
5. **Garde-fous** : budget dur par tick de décision ; une erreur Lua →
   LOG + retombée sur `chooseCombatMove` (le boss reste fonctionnel en
   « soldat de base » plutôt que planté) ; le script ne touche NI la
   perception ni le blade-sweep (réflexes C++).

## Ce qu'on ne fera PAS

- Un `think()` Lua par frame et par PNJ (boucle chaude).
- Un moteur de behavior trees parallèle (la décision reste UNE fonction,
  C++ par défaut, Lua par exception).
- Des écritures d'attributs depuis le script (§2.9 : abilities/effects
  seulement).

## Briques d'implémentation (post-P0, à ordonnancer)

1. Les événements de combat sur le bus (C++ pur, ~petite) — valeur
   immédiate pour quêtes/cues même sans Lua.
2. `ActorForm.brainScript` + le tick de décision, retours MOVES
   uniquement (le boss « décide » mais n'a pas encore d'actions).
3. Les bindings d'actions + `wait()` dans les décisions.
4. Un boss de démonstration 100 % data+Lua (la preuve moddeur).
