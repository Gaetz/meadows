# Sortilèges — référence de design

> Référence canonique du système de sortilèges (chantier ESPRITS,
> `docs/CHANTIER-ESPRITS.md`). Cadre posé par le dev le 2026-09-27 ; la
> mise en œuvre v1 (les deux sorts CRÉER + EAU) est décrite ici telle
> qu'elle est codée. **À lire avant de toucher `world/spirit/Spells`,
> `LandscapeScene::castSpirit/executeSpell` ou `spirits.toml`.**
> Doc moddeur (anglais) : `userdoc/spirits.md`.

---

## 1. L'idée

Un **sortilège** est **une forme appliquée à un élément**, qualifiée par
des caractéristiques (portée, trajectoire, intensité, durée, zone,
maintien…). Tout est donnée : un sort = des records, aucun code par sort.
Les combinaisons de formes et d'éléments viendront plus tard ; on commence
par des sortilèges **simples** (une forme, un élément).

### Les cinq formes

| Forme | Code (`form`) | Ce qu'elle fait à l'élément |
|---|---|---|
| **Créer** | `create` | fait apparaître l'élément dans le monde (une source d'eau, un feu, une rafale…) |
| **Détruire** | `destroy` | retire l'élément (assécher, éteindre, dissiper) |
| **Transformer** | `transform` | change l'état de l'élément (eau → glace, terre → sable, lumière → ténèbres) |
| **Contrôler** | `control` | manipule et déplace l'élément existant (pousser l'eau, sculpter la terre, courber une flamme) |
| **Comprendre** | `understand` | donne des informations (où est l'eau, ce qui brûle, ce qui écoute) — pas d'effet sur le monde |

### Les neuf éléments

Les neuf esprits (`SpiritKind`, `engine/terrain/SpiritField.hpp`), en
trois triades shifumi : `Vegetation`, `Light`, `Darkness` / `Earth`,
`Lightning`, `Psy` / `Water`, `Fire`, `Wind`. L'élément d'un sort est un
nom d'esprit.

---

## 2. Les caractéristiques d'un sortilège

| Caractéristique | Champ | Unité / valeurs | Rôle |
|---|---|---|---|
| Forme | `form` | une des cinq | le verbe |
| Élément | `element` | un nom d'esprit | le sujet |
| **Trajectoire** | `trajectory` | `self` \| `point` \| `stream` \| `projectile` | où l'effet se produit : sur le lanceur ; au point de sol visé ; en **flux** continu depuis la main le long de la visée (suit la visée) ; en **un** arc depuis la main, l'effet là où il retombe |
| **Portée** | `range` | mètres | `point` : distance de visée maximale ; `stream`/`projectile` : portée balistique à 45° sur sol plat — la vitesse de lancement en découle (`v = √(g·portée)`, 20 m ⇒ 14 m/s) |
| **Intensité** | `intensity` | unité de l'élément par seconde (eau : m³/s) | la puissance de l'effet |
| **Durée** | `duration` | secondes (**secondes-sim** pour ce qui alimente une simulation) ; `-1` = permanent | combien de temps l'effet persiste |
| **Zone** | `areaShape` + `areaRadius` | `disc` \| `ring`, mètres | l'empreinte au point d'effet — v1 : un disque = le disque propre du noyau d'eau (~4 m), un anneau = la « large source » à sept prises pour un rayon > 4 m |
| **Maintien** | `channeled` + `costPeriod` | bool, secondes | maintenu = le sort vit tant que la touche est tenue ; le coût de l'ability est **repayé** toutes les `costPeriod` s (§2.9 : par l'effet de coût) ; relâcher, ou ne plus pouvoir payer, l'arrête |

Ce qui reste **sur l'ability** (§6 du CLAUDE.md), pas sur le sort : le
**coût** (un EffectForm), le **cooldown** (un EffectForm à tag), les
**tags** requis/bloquants (`State.Exhausted`…), les **conditions**
(ConditionForm enfants) et le **skill** entraîné. L'ability est
l'*activation* ; le sort est l'*effet sur le monde*.

### Suggestions de caractéristiques à venir (à trancher par le dev)

- **Temps d'incantation** (`castTime`) — un délai avant l'effet, interruptible.
- **Dispersion** (`spread`, degrés) — la largeur du cône d'un flux ou d'un souffle.
- **Cible** (`target` : `ground` \| `actor` \| `object` \| `any`) — ce que la
  visée accepte ; aujourd'hui `point` n'accepte que le sol du terrain.
- **Persistance** — déjà couverte par `duration = -1` (source permanente).
- **Élément secondaire** — pour les combinaisons (eau + froid = glace,
  terre + feu = lave) : le jour venu, un second `element` et une règle de
  la table BotW (`SpiritRuleForm`) pour dire ce que la paire produit.
- **Puissance vs intensité** — si un jour la puissance du lanceur (stat
  Essence, skill) doit moduler l'effet, c'est un *multiplicateur* appliqué
  à `intensity`/`range`/`duration` au moment du cast, pas un champ de
  plus : le record reste la définition, le lanceur reste la modulation.

---

## 3. Le modèle de données

```
AbilityForm (l'activation : coût, cooldown, tags, conditions, skill)
   └── SpellForm (enfant, `parent` = l'ability) : forme + élément + caractéristiques
```

- **`SpellForm`** (`data/forms/SpiritForms.hpp`) est un **record enfant** de
  l'`AbilityForm` (le pattern `parent` déjà utilisé par `ConditionForm`
  et les records enfants de quêtes). Une ability sans enfant `SpellForm`
  reste une ability **scriptée** : `AbilityForm.script` (Lua : `aim()`,
  `spirit.spawn`, `spirit.jet`, `wait`) est la **soupape** pour ce que la
  matrice ne couvre pas encore.
- **Compilation** : `world::compileSpell(SpellForm) → SpellSpec` (POD)
  valide chaque champ énuméré par son nom et les bornes (durée > 0 ou −1,
  rayon > 0, `costPeriod` > 0 si maintenu). `world::spellSupported(spec)`
  est **la matrice** : les cases (forme, élément, trajectoire) que le
  lanceur générique sait exécuter.
- Un sort est donc moddable comme tout Form (§5) : patcher `intensity`
  d'un sort de base, ajouter un sort = une ability + un enfant.

---

## 4. La matrice v1 — ce qui est exécuté

| Forme × Élément | `self` | `point` | `stream` | `projectile` |
|---|---|---|---|---|
| Créer × Eau | — | ✅ **source** posée au sol visé (`SpellWaterSpring`) | ✅ **jet** depuis la main, suit la visée, maintenu (`SpellWaterStream`) | ⏳ |
| Créer × Terre | — | ⏳ (E2 : `push_terrain`) | — | — |
| Créer × Feu | — | ⏳ (E3) | ⏳ | ⏳ |
| Créer × Vent | — | — | ⏳ (E4) | — |
| Détruire / Transformer / Contrôler / Comprendre × … | ⏳ | ⏳ | ⏳ | ⏳ |

Une case hors matrice refuse le cast (toast, log) **avant** de payer :
l'auteur du sort sait qu'il lui faut un script ou attendre la brique.

### Exécution (`LandscapeScene::castSpirit` → `executeSpell`)

1. Résolution : le premier enfant `SpellForm` de l'ability courante, compilé.
2. **Pré-vérifications géométriques avant de payer** (`tryActivate` n'a
   pas de position) : pour `point` — viser le sol du terrain (un rocher ou
   un tronc n'est pas du sol), à moins de `range` m, **sol sec** (verser
   dans un lac épinglé serait avalé par le pin). Toasts `spirit.noGround`,
   `spirit.tooFar`, `spirit.wetGround`. `stream` n'a rien à pré-vérifier :
   l'arc retombe où il retombe.
3. `tryActivate` (coût, cooldown, tags, conditions) ; refus = `spirit.refused`.
4. `executeSpell` : Créer + `point` → une **source** placée
   (`SpiritSourceForm` runtime, persistée par la save, expire à `duration`) ;
   Créer + `stream` → un **jet** (`world/spirit/SpiritJets`, transitoire :
   un geste, jamais sauvé) dont la vitesse vient de `range`, le débit de
   `intensity`, la vie de `duration`, le maintien de `channeled`.
5. Tout passe par la file `pendingSpiritActions` et le point sûr de la
   frame (jamais d'action monde au milieu d'une itération ECS).

### Maintien (`channeled`)

Sur un jet : tant que la touche est tenue, le jet vit (au plus `duration`
s) ; toutes les `costPeriod` s le coût de l'ability est repayé par
`gameplay::payAbilityCost` (le même EffectForm de coût, la même politique
strict/permissive) ; relâcher la touche, mourir, ou ne plus pouvoir payer
l'arrête. Une source `point` maintenue n'existe pas en v1 (une source est
posée dans le monde, pas tenue par la main) — à discuter si un cas le veut.

---

## 5. Les deux sorts v1 (`game/data/base/spirits.toml`)

```toml
# L'activation : essence -15 (strict), cooldown 8 s, bloquée par State.Exhausted
[[records]]
form = "5b1e1700-0000-4000-8000-000000000063"
type = "AbilityForm"
new = true
[records.fields]
editorId = "SpiritWater"
cost = "5b1e1700-0000-4000-8000-000000000061"
cooldown = "5b1e1700-0000-4000-8000-000000000062"
costPolicy = "strict"
blockedTag = "State.Exhausted"

# L'effet : CRÉER + EAU au point visé
[[records]]
form = "5b1e1700-0000-4000-8000-000000000081"
type = "SpellForm"
new = true
[records.fields]
editorId = "SpellWaterSpring"
parent = "5b1e1700-0000-4000-8000-000000000063"
form = "create"
element = "Water"
trajectory = "point"
range = 24.0        # m de visée
intensity = 3.0     # m³/s
duration = 10.0     # s-sim
areaShape = "disc"
areaRadius = 4.0
channeled = false
```

```toml
# CRÉER + EAU en flux maintenu : essence -10 à l'activation puis -10/s
[[records]]
form = "5b1e1700-0000-4000-8000-000000000082"
type = "SpellForm"
new = true
[records.fields]
editorId = "SpellWaterStream"
parent = "5b1e1700-0000-4000-8000-000000000066"   # SpiritWaterJet
form = "create"
element = "Water"
trajectory = "stream"
range = 20.0        # ⇒ 14 m/s à 45°
intensity = 2.0     # m³/s au point de chute
duration = 6.0      # maintien maximal
areaShape = "disc"
areaRadius = 2.0
channeled = true
costPeriod = 1.0
```

En jeu : **Q** lance l'ability courante (`SpiritWaterJet` par défaut) ;
console `spirit cast <EditorId>` pour en choisir une autre tant qu'il n'y
a pas de barre de sorts.

---

## 6. Ce qui reste ouvert (décisions dev)

1. **Sélection des sorts** : une barre (1-9 ?), une roue radiale, ou la
   combinaison forme + élément choisie à la volée (deux touches) — ce
   dernier point rejoint « les combinaisons » et le rendu « incantation ».
2. **Coût dérivé ou autoré ?** Aujourd'hui le coût est un EffectForm sur
   l'ability (autoré). Une formule `coût = f(intensité, durée, portée)`
   est possible plus tard (un calculateur, les records restent la vérité).
3. **`self` et `projectile`** : à ouvrir avec le premier sort qui en a
   besoin (Comprendre × … est probablement `self` ; une boule de feu est
   `projectile`).
4. **Détruire × Eau** = une source à débit **négatif** (le noyau borne à
   zéro depuis E1.a) — quasi gratuit, bon premier sort « Détruire ».
5. **Contrôler × Terre** = le pinceau de sculpt (E2) ; **Contrôler × Eau**
   = pousser l'eau (une source de vent local sur la sim ?) — à trancher
   avec la brique vent.
