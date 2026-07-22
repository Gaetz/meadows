# Followers

Les suivants (*Followers* en anglais) sont des personnages qui peuvent accompagner le joueur dans ses aventures. Ils peuvent développer une relation avec le joueur et évoluer.

## 1. Intention

L'objectif des followers est de créer une fantaisie d'amitié avec des personnages forts et évolutifs dans le monde du jeu. Le joueur va vivre ses aventures avec les followers et s'attacher à eux. Il va aussi devoir prendre soin d'eux, parce que les features de survie et de blessure sont aussi actives pour eux, ce qui oblige à les laisser se reposer parfois — et donc changer de follower et nouer de nouvelles relations d'amitié.

Chaque follower est personnalisé, au-delà de son apparence. En plus de ses caractéristiques, le follower a un pouvoir spécial qui lui est propre et renforce son identité.

La mort d'un follower crée une tombe dans le monde du jeu, sur laquelle le joueur peut déposer des objets ou des fleurs.

**Points dev :**

- Système de followers comme compagnons persistants accompagnant le joueur
- Système de relation joueur-follower (attachement, évolution dans le temps)
- Appliquer les systèmes de survie et de blessure aux followers (réutiliser les systèmes joueur)
  - Le follower doit se reposer quand sa survie/blessure l'exige → indisponibilité temporaire
  - Implique un mécanisme de rotation : le joueur doit changer de follower actif
- Personnalisation de chaque follower
  - Apparence unique
  - Caractéristiques propres (stats, personnalité)
  - Pouvoir spécial unique par follower (activation, effet, intégration dans le gameplay)
- Système de quêtes spécifiques par follower, réparties sur la durée du jeu
- Système de dialogues propre à chaque follower
  - Conditions de déblocage progressives liées aux évènements du jeu
- Évolution du follower au contact du joueur
  - Apprentissage de nouvelles compétences
  - Transformation du pouvoir spécial ou apprentissage d'un pouvoir alternatif
- Le joueur peut débloquer des perks au contact d'un follower (système réciproque)
- Mort du follower
  - Spawn d'un objet « tombe » persistant dans le monde
  - Interaction avec la tombe : déposer des objets ou des fleurs (UI de dépôt)
- Gestion multi-followers : framework prévu pour en gérer plusieurs simultanément (cap configurable)
- Home bases / bivouacs : un follower peut avoir un lieu d'attache (taverne, maison, camp) où il retourne entre deux recrutements
- Commandes de groupe : « tous suivre », « tous rester », « tous attaquer », « tous se défendre », avec raccourcis radial menu
- Narratif follower : chaque follower majeur porte un petit arc narratif indépendant (origin story, motivation, objectif long terme)
- Déblocages progressifs : certaines interactions/quêtes/perks sont débloquées selon le niveau d'affinité joueur-follower (Approval / Regard)

**Références :** <https://www.nexusmods.com/skyrimspecialedition/mods/55653>

## 2. Stats, classe, compétences, survie, mort des followers

Les followers ont le même set de stats que le joueur. Ils commencent avec un niveau minimum qui dépend de leur statut et de leur rôle dans la société. Contrairement aux joueurs, les followers ont une classe, qui sert à déterminer leurs statistiques en fonction de leur niveau.

Les stats du follower sont affichées quand on lui propose un recrutement, avant validation.

En plus de leurs stats, les followers ont un âge. Un âge avancé diminue deux multiplicateurs (inférieurs à 1) qui impactent leurs compétences pour simuler le vieillissement physique ou la sénilité.

Contrairement au joueur, les compétences des followers dépendent uniquement de leur niveau, via leur classe. Le niveau des followers fait augmenter leurs compétences et leurs stats selon des courbes prédéfinies par la classe. La classe détermine aussi le comportement en combat.

Le niveau des followers augmente avec celui du joueur. Quand un follower n'accompagne plus le joueur, son niveau augmente seulement de la moitié de celui du joueur quand le joueur le retrouve. Il y a néanmoins des exceptions pour les personnages principaux de l'histoire qui peuvent éventuellement retrouver le joueur avec un niveau égal ou supérieur plus loin dans l'histoire, afin qu'ils restent pertinents dans la région où ils peuvent accompagner le joueur.

Chaque follower a un niveau d'affinité avec le joueur. Ce niveau d'affinité augmente doucement avec le temps passé avec le joueur, et peut aussi évoluer positivement et négativement avec certaines actions spécifiques au personnage. Ce niveau d'affinité est une condition de déclenchement pour certaines interactions. Chaque follower a aussi un niveau d'affinité avec les autres followers, qui sert aussi au déclenchement de certaines interactions.

Les followers sont impactés par les stades de survie de la même manière que le joueur. Néanmoins, ils ne peuvent pas mourir dans des conditions trop extrêmes : cela rendrait le jeu trop difficile et frustrant. Les followers peuvent être victimes de blessures tout comme le joueur. Le joueur peut, en discutant avec le follower, connaître l'état de ses blessures et visualiser ses statistiques impactées par les variables de survie. La guérison des blessures prend un certain temps que le joueur peut connaître. Subir une blessure alors que le follower est déjà blessé peut entraîner un risque de blessure plus grave ou de mort. Le joueur doit donc attendre que le follower se rétablisse ou utiliser un autre follower pendant que son précédent follower se rétablit.

À la mort d'un follower, celui-ci peut être enterré sur place. Le joueur peut aussi décider d'emporter son cadavre pour l'enterrer à un autre endroit. Enfin, le joueur peut demander à un proche du follower d'aller récupérer son cadavre, auquel cas le follower est enterré dans un lieu prédéfini, correspondant à son historique. Le joueur peut accéder à l'inventaire du follower mort en utilisant l'interaction spéciale avec la tombe du follower, comme il le faisait quand le follower était vivant. Une interaction normale avec la tombe du follower déclenche une animation d'hommage.

Le joueur peut avoir jusqu'à 5 followers majeurs (personnages), plus 6 followers mineurs tels que des animaux, constructions, familiers ou invocations.

Les montures ne comptent pas comme des followers majeurs ou mineurs, même si elles ont des comportements similaires. Elles sont associées au joueur ou à des followers majeurs. Elles ont un paragraphe dédié plus bas.

**Points dev :**

- Stats des followers : même set de stats que le joueur
  - Niveau minimum initial par follower, configuré dans les data (dépend du statut/rôle social)
  - Afficher les stats du follower quand on veut le recruter
  - UI de cet affichage
- Probabilité de mort réelle : en cas d'état normal, en cas de blessure (selon la gravité)
  - Friendly fire paramétrable : seuils de dégâts (magie, flèches, AoE) au-dessus desquels le follower réagit (commentaire, baisse d'affinité, fuite)
  - Soin « E to heal » : interaction contextuelle (touche d'activation) pour relever un follower à terre, avec cooldown et consommation de potion
  - Gestion de l'incapacité : animation « à genoux » visible, timer de récupération, possibilité d'abandonner le follower si trop blessé
- Système de classes pour les followers (le joueur n'a pas de classe)
  - Chaque classe définit des courbes de progression : niveau → stats et compétences
  - Les compétences du follower sont entièrement déterminées par le niveau et la classe (pas de répartition manuelle)
  - Chaque classe correspond à un certain style de combat
- Système d'âge du follower
  - Deux multiplicateurs de vieillissement (valeurs < 1.0) : un physique, un mental/sénilité
  - Appliqués sur les compétences effectives : `compétence_effective = compétence_base × multiplicateur`
- Leveling lié au joueur
  - Follower actif : son niveau suit celui du joueur
  - Follower inactif : au moment des retrouvailles, gain = 50 % du gain accumulé par le joueur entre-temps
  - Exception pour les personnages principaux de l'histoire (flag dans les data) : peuvent rattraper le niveau du joueur (≥) pour rester pertinents dans leur région
- Système d'affinité joueur-follower
  - Augmentation passive proportionnelle au temps passé ensemble
  - Modification positive/négative par actions spécifiques (configurables par follower)
  - Sert de condition de déclenchement pour certaines interactions/dialogues
- Système d'affinité inter-followers (matrice N×N)
  - Sert de condition de déclenchement pour les interactions entre followers
- Survie et blessures
  - Appliquer les stades de survie du joueur aux followers (faim, soif, fatigue, etc.)
  - Protection : les followers ne peuvent pas mourir de conditions extrêmes de survie (seuil plancher)
  - Système de blessures identique au joueur
  - Dialogue de consultation : le joueur peut voir l'état de blessure et les stats impactées
  - Timer de guérison par blessure (affiché au joueur)
  - Blessure sur follower déjà blessé → probabilité configurable de blessure aggravée ou mort
  - Convalescence implique indisponibilité → rotation vers un autre follower
- Mort et enterrement — trois options :
  - Enterrer sur place → spawn tombe à la position courante
  - Emporter le cadavre (objet dans l'inventaire joueur) → enterrer à un lieu choisi
  - Demander à un PNJ « proche » du follower → le proche récupère le corps, tombe dans un lieu prédéfini (data du follower : lieu d'enterrement par défaut, PNJ proche associé)
- Tombe du follower mort
  - Interaction spéciale → accès à l'inventaire du follower mort
  - Interaction normale → animation d'hommage
  - Persistance en monde ouvert (sauvegarde)
- Composition du groupe
  - Maximum 5 followers majeurs (personnages)
  - Maximum 6 followers mineurs (animaux, constructions, familiers, invocations)
  - Catégorisation majeur/mineur dans les data de chaque follower
  - UI de gestion du groupe reflétant les deux catégories
  - Les montures sont un type spécifique de follower, associé au joueur ou à un follower majeur, et qui ne comptent pas dans la limite

## 3. Perks, pouvoirs spéciaux et évolution des followers

En fonction de leur niveau de classe, les followers acquièrent des perks. Chaque follower a aussi un pouvoir spécial, qu'il utilise comme une perk.

Le joueur a une influence sur l'évolution des followers. À chaque fois que le joueur passe un niveau, le follower acquiert un point de compétence dans la compétence la plus élevée du joueur, si cette compétence est supérieure à celle du follower. Si ce n'est pas le cas, c'est la deuxième compétence la plus élevée du joueur qui est utilisée, et cetera.

Le pouvoir spécial du follower peut évoluer en fonction soit d'éléments scénaristiques, notamment un choix qui est fait par le joueur dans sa relation ou dans le scénario spécifique au follower, soit en fonction des perks acquises par le joueur. Un dialogue spécial est débloqué quand les conditions d'acquisition de cette évolution du pouvoir spécial sont satisfaites.

Inversement, le joueur peut débloquer des perks au contact du follower. Là encore le joueur doit répondre à un certain nombre de conditions, souvent scénaristiques, ou liées à des actions du joueur ou au temps passé avec le follower. Un dialogue spécial se débloque pour permettre au joueur d'apprendre ses perks. Le dialogue peut nécessiter d'être dans un endroit tranquille ou dans un endroit particulier, ce qui est signifié au joueur.

**Points dev :**

- Rôles tactiques assignables : tank, DPS, support, healer, archer, mage — influence l'IA de combat (positionnement, ciblage, sorts utilisés)
- Métriques relationnelles : Approval (approuve/désapprouve les actions), Bravery (fuite en combat), Regard (affinité long terme) — visibles dans un menu dédié
- Perks de classe du follower
  - Table de déblocage : niveau de classe → perks débloquées (configurable par classe)
  - Le pouvoir spécial est intégré comme une perk spéciale (slot unique ou flag)
- Influence du joueur sur l'évolution des compétences du follower
  - Au level-up du joueur : +1 point de compétence au follower actif
  - Algorithme d'attribution : parcourir les compétences du joueur par ordre décroissant, attribuer le point dans la première dont la valeur joueur > valeur follower
  - Si aucune compétence du joueur ne dépasse celles du follower : pas de point attribué (cas implicite à gérer)
- Évolution du pouvoir spécial du follower — deux voies :
  - Via un choix scénaristique du joueur (relation avec le follower ou scénario spécifique)
  - Via les perks acquises par le joueur (vérification de la liste de perks du joueur)
  - Conditions d'évolution configurables par follower dans les data
  - Dialogue spécial de déblocage quand les conditions sont satisfaites
- Perks apprises par le joueur au contact d'un follower (système réciproque)
  - Conditions configurables : avancement scénaristique, actions spécifiques, temps passé ensemble
  - Dialogue spécial d'apprentissage
  - Contrainte de lieu : nécessite un endroit « tranquille » (tag de zone) ou un lieu spécifique
  - Notification au joueur quand le dialogue est disponible mais le lieu ne convient pas encore
- IA furtivité : le follower se planque correctement quand le joueur est en sneak, ne déclenche pas la détection, imite les déplacements

## 4. Condition d'accès aux followers

Les followers n'acceptent pas forcément de suivre le joueur. Les conditions peuvent être :

- Une condition de niveau
- Une condition de réputation
- Une condition concernant plusieurs compétences
- Une condition de scénario

Par ailleurs, certains followers sont des mercenaires et acceptent de suivre le joueur en étant payés pour une certaine durée (par exemple une semaine). Le prix peut dépendre de la réputation du joueur, de ses factions, de son niveau, de son degré de richesse.

Quand les conditions pour que le follower suive le joueur ne sont pas remplies, le follower le spécifie via une réponse spéciale quand le joueur essaye de le recruter.

**Points dev :**

- Système de conditions de recrutement par follower (combinaison configurable dans les data)
  - Condition de niveau minimum du joueur
  - Condition de réputation (seuil, potentiellement par faction)
  - Conditions sur plusieurs compétences (seuils individuels par compétence)
  - Condition de progression scénaristique (flags de quête / état du scénario)
  - Conditions narratives : certains followers exigent une quête d'introduction spécifique avant d'accepter de suivre
  - Conditions de guilde/faction : appartenance à une faction (Compagnons, Collège, Voleurs) peut débloquer ou bloquer certains recrutements
  - Conditions d'équipement : certains followers refusent si le joueur ne porte pas une armure minimum
  - Re-recrutement : follower congédié retourne à sa home base, peut être re-recruté plus tard (pas de lockout définitif)
  - Les conditions sont combinées (AND) : toutes doivent être satisfaites
- Type « mercenaire »
  - Contrat à durée déterminée (durée en jours in-game, ex : 7 jours)
  - Timer de contrat avec notification d'expiration proche et expiration
  - Calcul dynamique du prix : `f(réputation, factions, niveau, richesse du joueur)` — formule configurable par mercenaire
  - Gestion de la fin de contrat : le mercenaire quitte le groupe, possibilité de renouvellement
- Dialogue de refus contextuel en cas de conditions non remplies
  - Identifier quelle(s) condition(s) échoue(nt)
  - Sélectionner le dialogue de refus correspondant (donne un indice au joueur sur ce qui manque)
  - Dialogues de refus enrichis : chaque condition non remplie a un dialogue spécifique donnant un indice actionnable au joueur

## 5. Équipement du follower

Le follower peut porter les mêmes équipements que le joueur. Il possède un équipement minimal qu'il est impossible de lui enlever, mais peut utiliser l'équipement que le joueur lui fournit, si ses stats le lui permettent et que ses compétences le rendent plus intéressant. Il ramasse le butin qui l'intéresse.

Le joueur peut accéder facilement à l'équipement du follower pour le modifier (Shift + interaction). Si le follower a un avis négatif sur le joueur, cette interaction est impossible.

Le poids que le follower peut porter est limité par ses caractéristiques et son modificateur d'âge.

Le follower peut atteindre un niveau où son équipement de base devient obsolète. Se débloque alors un dialogue, si le joueur et le follower se trouvent dans une ville qui dispose d'une forge, qui demande au joueur une certaine somme pour améliorer l'équipement de base. Le follower se rend alors à la forge pour faire améliorer son équipement. Le follower peut aussi améliorer automatiquement son équipement s'il a atteint le bon niveau et n'est pas avec le joueur pendant trente jours en jeu. Les mercenaires ne demandent pas cette somme, juste d'aller à la forge améliorer leur équipement. On considère que leur salaire suffit à l'entretien de leur équipement.

Entretien de l'équipement : on considère que le follower répare son équipement lui-même quand il passe près d'une forge.

**Points dev :**

- Réutiliser le système d'équipement du joueur pour les followers (mêmes slots, mêmes types d'items)
  - Auto-loot : le follower ramasse du butin qui l'intéresse si le joueur ne le ramasse pas lui-même. Un dialogue poli a lieu à ce moment-là
- Équipement de base par follower
  - Flag « non-retirable » sur les items de l'équipement minimal
  - Les stats de cet équipement de base évoluent (voir amélioration ci-dessous)
- Conditions d'équipement d'un item fourni par le joueur
  - Vérifier les prérequis de stats du follower
  - L'item doit être plus intéressant que l'équipement actuel (comparaison via les compétences du follower)
- Accès à l'inventaire/équipement du follower
  - Input : Shift + touche d'interaction
  - Bloqué si l'affinité/avis du follower envers le joueur est négatif (dialogue de refus)
  - Auto-équipement intelligent : le follower équipe automatiquement les améliorations d'équipement que le joueur lui donne, selon son rôle/stats
- Poids transportable
  - Calcul : `f(caractéristiques du follower) × multiplicateur d'âge`
  - Vérification de surcharge à chaque ajout d'item — les items qui dépasseraient le poids limite ne peuvent être ajoutés
- Amélioration de l'équipement de base
  - Paliers d'obsolescence par follower (seuils de niveau dans les data)
  - Déclenchement du dialogue d'amélioration : niveau atteint + présence dans une ville avec forge (tag « forge » sur les villes)
  - Coût en or (configurable par follower/palier)
  - Animation/action : le follower se rend à la forge (pathfinding NPC vers le POI forge)
  - Amélioration automatique hors-groupe : si le follower a atteint le bon niveau et est absent du groupe pendant ≥ 30 jours in-game, l'amélioration s'applique au retour
  - Exception mercenaires : pas de coût (le salaire couvre l'entretien), mais nécessitent quand même d'aller à la forge
- Entretien / réparation d'équipement
  - Réparation automatique quand le follower passe à proximité d'une forge (trigger spatial / zone)
  - Implique un système de durabilité sur l'équipement des followers (ou hérité du système joueur)

## 6. Interactions des followers

### 6.1 Interaction avec le joueur

De manière générale, les followers suivent le joueur. S'ils sont trop éloignés, ils ont un bonus de vitesse de 25 % pour le rattraper. Ils apparaissent près du joueur lors des changements de maps. Ils apparaissent à une certaine distance du joueur à un point qui rend le joueur accessible, s'ils se sont trop éloignés ou que le joueur est inaccessible.

Au cours du jeu, chaque follower peut déclencher des interactions avec le joueur.

- À certains moments de scénario ou à certains lieux ou types de lieu, le follower peut s'exprimer sur ce qu'il ressent ou commenter la situation. Cela peut déclencher une interaction entre followers éventuellement (cf. ci-dessous).
- En fonction de ses compétences, de son niveau et de son affinité avec le joueur, le follower peut déclencher certains dialogues ou encourager le joueur / se moquer de lui dans certaines activités. Les conditions de déclenchement peuvent comprendre les compétences ou le niveau du joueur.
- Quand le joueur fait autre chose, par exemple quand il est dans un bâtiment de commerce, une taverne ou qu'il fait de l'artisanat, les followers majeurs divaguent, exécutent leurs occupations, vont éventuellement s'asseoir, se reposer ou faire leurs courses dans la ville. Le follower peut aussi déclencher des évènements ou dialogues uniques avec les NPC ou autres followers, ce qui renforce sa personnalité.
- Favors ponctuels : le follower demande de petits services au joueur (chercher une plante, livrer un message), non-bloquants mais narratifs et peuvent fournir une récompense utile.

Certaines interactions doivent avoir lieu dans un certain ordre. Les conditions de déclenchement de certaines interactions comprennent l'exécution d'une interaction précédente, avec un temps d'écart minimum entre la précédente et la suivante. Ces actions ne peuvent pas se déclencher quand le joueur est en mode discrétion.

Pour éviter qu'une interaction se répète trop souvent, il existe un timer sur les interactions. Par défaut, il est réglé sur 10 h de jeu. L'interaction ne se répètera pas tant que ce timer ne sera pas écoulé. Certaines interactions ne peuvent pas se répéter.

Pour les animaux/familiers, le joueur peut donner des ordres particuliers.

### 6.2 Interactions entre followers

Quand plusieurs followers sont dans le groupe du joueur, plusieurs interactions peuvent se présenter :

- Deux followers peuvent déclencher un dialogue entre eux, s'ils sont présents au même évènement de scénario, ou régulièrement au cours du jeu.
- Si un follower est à terre et qu'un autre a des objets de guérison, il peut en utiliser un pour sauver le premier follower — ou le joueur d'ailleurs. Il doit d'abord se débarrasser des ennemis qui se battent au corps à corps.
- Un combat ne finit pas tant qu'un follower peut soigner d'autres membres du groupe grâce à un objet de soin. Il garde toujours un objet de soin pour pouvoir soigner le joueur en priorité.
  - **Déviation implémentée (Maela)** : Maela soigne par POUVOIR, pas par
    objet — elle n'a pas de stock à réserver, la règle « garde un objet en
    réserve » ne s'applique donc volontairement pas à elle
    (`game/scenes/NpcCombatController.cpp`).

**Points dev :**

- Système d'interactions contextuelles follower → joueur
  - Déclencheurs multiples : évènement scénaristique, lieu spécifique, type de lieu (tag sur la zone)
  - Le follower commente ou exprime un ressenti → ligne de dialogue contextuelle
  - Certaines de ces réactions peuvent chaîner vers une interaction inter-followers
  - Pas d'interaction en mode discrétion
  - Systèmes des demandes/récompenses des followers
- Conditions de déclenchement des dialogues follower-joueur (combinables)
  - Compétences du follower
  - Niveau du follower
  - Affinité follower-joueur
  - Compétences du joueur
  - Niveau du joueur
  - Activité en cours du joueur (pour les encouragements/moqueries)
  - Outil d'intégration des doublages
- Activité des followers majeurs quand le joueur fait autre chose
  - Conditions de déclenchement des activités. Sandboxing contextuel : en ville le follower fréquente tavernes/commerces, en donjon il inspecte le décor, en nature il chasse/cueille
  - Tag des maps / endroits où ces activités sont possibles
  - Activités possibles en fonction des compétences, de l'état de l'équipement, des stats, de ce qui est disponible à proximité
  - Condition de déclenchement de dialogues / évènements uniques
  - Réactions au craft : commentaires uniques quand le joueur utilise forge, alchimie, enchantement, cuisine, etc.
  - Ordres pour animaux : assis, couché, attaque, rapporte (pour chiens/loups), mode garde statique
- Chaînage ordonné d'interactions
  - Prérequis : une interaction précédente doit avoir été complétée
  - Cooldown minimum entre deux interactions chaînées (temps in-game configurable)
  - Data : graphe d'ordre des interactions par follower
- Système anti-répétition
  - Timer de cooldown par interaction (défaut : 10 h de jeu, configurable)
  - Tracker : timestamp de dernière exécution par interaction par follower
  - Flag « one-shot » : certaines interactions ne se jouent qu'une seule fois
- Dialogues inter-followers
  - Conditions : au moins deux followers présents dans le groupe
  - Déclenchement par évènement scénaristique commun ou timer régulier de dialogue
  - Matrice de dialogues follower × follower (les paires doivent avoir du contenu)
- IA de soin inter-followers en combat
  - Détection d'un allié à terre (follower ou joueur)
  - Vérification de l'inventaire : possède un objet de soin ?
  - Priorité comportementale : se débarrasser des ennemis au corps à corps d'abord, puis soigner
  - Réserve obligatoire : toujours garder au moins un objet de soin pour le joueur (priorité joueur)
  - Peut soigner un follower allié ou le joueur
- Condition de fin de combat étendue
  - Le combat ne se termine pas tant qu'un follower peut encore soigner un allié à terre avec un objet de soin

## 7. Interaction en combat

Les followers ont une stratégie de combat par défaut, qui dépend de leur classe. Cette stratégie est modifiable par le joueur via les dialogues.

De manière générale, les followers engagent le combat si on attaque le joueur (hors combat déclenché par le jeu comme une épreuve amicale, type brawl ou tournoi).

Quand le follower n'a plus de points de vie en combat, il est considéré comme étant à terre. Le joueur ou un autre follower peut le relever en lui fournissant un objet de soin.

**Points dev :**

- Stratégie de combat par défaut par classe (IA comportementale : tank, support, DPS distance, DPS mêlée, etc.)
  - Configurable dans les data de chaque classe
- Modification de la stratégie de combat par le joueur via le système de dialogue
  - Options de stratégie proposées au joueur (liste définie par classe ou globale)
  - Persistance du choix de stratégie par follower (sauvegardé)
- Déclenchement automatique du combat quand le joueur est attaqué
  - Exception : ne pas engager le combat lors d'épreuves amicales (brawl, tournoi)
  - Flag « épreuve amicale » sur les combats scriptés → les followers restent passifs
- État « à terre » (downed) quand PV = 0
  - Le follower est immobilisé, ne peut plus agir, mais n'est pas mort
  - Distinct de la mort permanente (la mort intervient dans d'autres conditions, cf. section 2)
- Mécanique de relèvement
  - Le joueur ou un autre follower utilise un objet de soin sur le follower à terre
  - Animation de relèvement
  - Restauration des PV (montant à définir : partiel ou lié à l'objet de soin utilisé)

## 8. Montures et animaux de compagnie

Les montures sont un type spécifique de follower (qui ne compte pas dans la limite) permettant de se déplacer plus rapidement et de transporter du matériel. Elles sont achetables à un vendeur de monture ou obtenables dans le jeu via des quêtes.

Les animaux de compagnie sont un type de followers qui compte dans la limite de followers mineurs mais ne permettent pas de les chevaucher.

Ils ont un niveau, des stats classiques et des compétences (pour le combat, la plupart du temps combat à mains nues). Leur niveau n'augmente pas avec celui du joueur. Si le joueur veut une monture ou un animal de compagnie plus fort, il faut en trouver un autre ou, pour certains, les entraîner. Les montures et les animaux de compagnie ont un niveau maximum. Ils ne peuvent dépasser le niveau courant du joueur. Le charisme des montures s'ajoute à celui du personnage quand il parle depuis sa monture ou se trouve proche d'elle.

Les montures ont par ailleurs des stats propres à leur usage et peuvent être équipées d'équipements spéciaux qui renforcent ces stats. Les statistiques sont les suivantes :

- Poids porté maximum (hors joueur)
- Vitesse de course (les vitesses de marche et de trot sont des divisions de cette vitesse)
- Courage (probabilité de panique/fuite en combat)

**Références :** <https://www.nexusmods.com/skyrimspecialedition/mods/169335>

### 8.1 Monture du joueur

Le joueur ne peut sélectionner qu'une monture principale à la fois, même s'il a la possibilité de choisir laquelle est sa monture principale s'il en a plusieurs, dans l'écurie, le magasin qui vend les montures.

Le joueur peut changer l'équipement spécifique de sa monture dans l'écurie.

Le joueur peut modifier ce que porte sa monture en utilisant l'interaction alternative pour accéder à son inventaire.

Il peut appeler sa monture en sifflant. Cela fonctionne si la monture est dans un rayon moyen autour du joueur. Sinon, la monture apparaît dans un endroit accessible pour le joueur et vient vers lui. Si le joueur se rend dans une écurie et que sa monture n'est pas dans le rayon autour du joueur, sa monture y sera toujours présente — ce que les PNJ de l'écurie signalent.

Le joueur peut apprendre des perks pour attaquer à dos de monture, avec des armes à une main, des sorts à une main et avec l'arc.

Le joueur peut cueillir plantes/fleurs/minerais depuis sa monture (sans descendre).

Animations de repos : la monture s'abreuve/mange automatiquement quand elle est stationnaire près d'eau/herbe.

### 8.2 Monture des followers

On part du principe que tous les followers disposent d'une monture. Idéalement c'est une monture personnalisée par follower, qui contribue à la personnalité du follower, mais sinon une monture random. Dans tous les cas, le joueur n'a pas besoin d'acheter des montures pour ses followers.

Les followers récupèrent leurs montures quand le joueur monte sur la sienne, et en descendent quand le joueur descend de sa monture. Ils suivent le joueur en cherchant à rester sur les routes jusqu'à ce que le joueur s'éloigne d'une certaine distance. La course ne consomme pas d'endurance sur la monture du follower. Par contre la vitesse s'applique, pour que le joueur qui trouve une monture rapide en voie l'intérêt.

Les montures des followers sont immortelles et fuient toujours au combat dès que le follower descend de monture — c'est-à-dire dès que le joueur descend de sa monture. (À moins que la monture soit spéciale, mais cela arrive peu souvent.)

**Points dev :** —

### 8.3 Montures au combat

Les montures pouvant mourir, ce qui laisse leur équipement sur leur corps ou dans leur tombe, il est plus pratique pour le joueur qu'elles évitent de mourir autant que possible. La plupart ont donc un comportement de fuite dès qu'un combat commence et que le joueur n'est pas dessus.

La plupart des montures ne sont pas entraînées au combat. Si elles subissent des dégâts, elles désarçonnent leur cavalier et s'enfuient ensuite. Certaines montures peuvent vouloir attaquer si leur cavalier n'est plus sur elles, dans ce cas elles s'enfuient seulement si le niveau de l'ennemi est bien supérieur au leur.

Retour automatique : monture fuyante revient vers le joueur après X secondes hors combat.

**Points dev :**

- Flag par monture : « monture de combat » vs « monture civile » (valeur par défaut)
- Comportement de fuite par défaut : au début d'un combat, si le cavalier n'est pas en selle, la monture fuit
- Mécanisme de désarçonnement : si la monture subit des dégâts alors qu'un cavalier est dessus, elle le jette au sol
- IA de contre-attaque optionnelle : certaines montures peuvent attaquer l'agresseur quand leur cavalier est démonté
- Arbitrage fuite/combat basé sur le niveau : si niveau ennemi >> niveau monture, fuite forcée
- Retour automatique : après X secondes hors combat, la monture fuyante revient vers son cavalier
- Paramètre X exposé dans le MCM (ou fichier de config)
- Mort de monture : gestion de corps / tombe avec équipement récupérable (réutilisation du système de tombe des followers)
- Flag d'immortalité par monture (utilisé par défaut pour les montures de followers, cf. §8.2)
- Événements / notifications au joueur : mort de monture, monture fuyante, monture de retour
