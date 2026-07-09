# Chantier 8 — Session de test dev (briques 8.7 → 8.11)

> Écrit 2026-07-10 (nuit). Tout ce qui suit est **construit, commité,
> 345 tests headless verts** — mais non validé à la main. La 8.6 est déjà
> validée. Ordre conseillé : la session suit le flux naturel (éditeur →
> quête des rations → en jeu). Scène : hub Demos → « Game DB (editor) » ;
> jeu : « Landscape ».

## 0. Le shell (8.7b) — 5 min

- [ ] Le jeu s'ouvre en **1920×1080**.
- [ ] La scène Game DB est UNE fenêtre dockée : Browser | Editor |
      Inspector ; détacher/re-docker un panneau à la souris ;
      `Windows > Reset layout` restaure le défaut.
- [ ] Menu `File` : nom + Export plugin ; `Edit` : Undo/Redo (**Ctrl+Z /
      Ctrl+Y**) ; `Windows` : Plugins, Console.
- [ ] La sélection dans l'inspecteur est **dorée** — même teinte que le
      cadre du nœud sélectionné dans le graphe.
- [ ] Les panneaux ImGui des **scènes 3D** n'ont pas changé (le passage
      au pin imgui-docking ne doit rien altérer visuellement).

## 1. Graphe de quêtes (8.7 + 8.7d) — 10 min

Catégorie **Quests** → « La menace de l'est » :
- [ ] La topologie se lit : états (badges bleus/vert/rouge, `<- start`),
      branches étiquetées par **le nom de leurs tâches** (« xN » si
      required > 1).
- [ ] **Drag pin→pin** entre deux états = un branch naît, sélectionné.
- [ ] **Drag pin→canvas vide** = menu « + State (branch from here) » ;
      l'état ET le branch naissent, la sélection tombe sur le branch.
- [ ] Clic droit état → **Set as start state** ; clic droit canvas →
      **+ State** ; `Suppr` refuse sur les records base (§5).
- [ ] Positions persistées entre deux lancements ; **Auto-layout** range.
- [ ] L'arbre de droite (hiérarchie) suit la sélection du canvas et
      inversement ; + State/+ Branch/+ Task y fonctionnent aussi.
- [ ] Warnings orange : branch sans destination, branch sans task,
      task écoutant un événement que rien n'émet.

## 2. Graphe de dialogues (8.7 + 8.7d) — 5 min

Catégorie **Dialogues** → le dialogue du Villager :
- [ ] L'arbre à plat : colonnes = profondeur, rangées = `order` ;
      speaker + extrait + badge « n cond ».
- [ ] Clic droit nœud → **+ reply** (speaker alterné) ; drag pin→vide →
      **+ reply here** à la position du relâchement.
- [ ] **Re-parenter** une réplique en tirant un lien → l'arbre suit ;
      re-parenter sous son propre descendant → rejet rouge + message.
- [ ] Badge « (!) no listener » sur un nœud dont l'événement ne
      déclenche rien.

## 3. Mots-clefs & pickers (8.7b polish + 8.7e + 8.10) — 5 min

- [ ] Tout champ à vocabulaire fermé est un **dropdown bleu Capitalisé** :
      `kind` d'un état de quête, `op`/`duration`/`buildupType` d'un
      effect, `kind` d'un AiPackage, `slot` d'une armure…
- [ ] Champs d'événement (`event`, `startEvent`) : combo bleu listant
      tous les noms en usage + `create "<nom>"` via le filtre.
- [ ] Champs d'items (`takeItem`, `rewardItem`, `item` de condition) :
      picker listant les 4 catégories (« editorId (Type) »).
- [ ] Guids typés : `clip` d'un état d'anim, `particles`/`sound` d'un
      cue, `cost`/`cooldown`/`effect` d'une ability, `package` d'une
      entrée de schedule → tous des pickers, plus aucun guid brut.

## 4. Articulation quêtes ↔ dialogues (8.7c/d/e) — 10 min

- [ ] Sélectionner le nœud « Puis-je aider le village ? » → section
      **Event: OnAcceptEasternMenace** avec « starts: La menace de
      l'est » ; navigation au clic.
- [ ] **Wire to a quest task…** sur un nœud de dialogue (labels
      « quête / task ») et **Wire to a dialogue option…** sur une task :
      les deux côtés reçoivent le même événement, généré si absent.
- [ ] **Start a NEW quest on this option** : quête + état initial créés
      et câblés, navigation vers la quête.

## 5. LA QUÊTE DES RATIONS (le test intégral) — 20 min

Créer, dans l'éditeur, sans TOML à la main :
1. Dialogue du garde : nœud « Rapporte-moi 3 rations » → réponse Player
   → **Start a NEW quest on this option**.
2. Sur la quête : drag depuis l'état initial → **+ State (branch from
   here)** → `kind = Success` sur le nouvel état.
3. Sur le branch : **+ Task** « Rapporter les rations » → **Wire to a
   dialogue option…** vers une nouvelle réplique « Voici les rations ».
4. Sur cette réplique : **+ condition** `HasItem` / TravelRations / 3 ;
   **+ condition** `HasTag` / `Quest.<TaQuête>.Active` ; champs
   `takeItem = TravelRations`, `takeCount = 3`.
5. Sur la quête : `rewardItem` (GoldCoin ?) + `rewardCount`.
6. **File → Export plugin** → relancer.
En jeu :
- [ ] Accepter → toast « Nouvelle quete : … » ; l'option n'est plus
      proposée une fois prise.
- [ ] L'option de rendu n'apparaît qu'avec 3 rations en poche ET la
      quête active.
- [ ] Rendre → **les 3 rations disparaissent**, la récompense tombe,
      toast « Quete accomplie : … (+N …) ».
- [ ] EasternMenace verse toujours ses **+50 or** (récompense désormais
      servie par les données).

## 6. Échec par dialogue (8.7e follow-up) — 5 min

- [ ] Ajouter à une quête un état `Failure` + branch + task câblée sur
      une option « J'abandonne » (gated `Quest.X.Active`) → en jeu :
      toast **« Quete echouee : … »**, le tag Active tombe, la quête ne
      redémarre jamais.

## 7. Timeline des clips (8.8) — 5 min

Catégorie **Anim Clips** → un clip :
- [ ] Les events en marqueurs sur la règle en secondes ; **tirer** un
      marqueur = retune de `time` (snap 0,01 s, UN undo par drag).
- [ ] **Clic sur un trou** = nouvel event à cet instant, sélectionné.
- [ ] `view (s)` ajuste la règle, `auto` la re-dérive des events.

## 8. Builder de conditions (8.9) — 5 min

Sur une réplique de dialogue (ou une ability) :
- [ ] Section **Conditions** : résumés lisibles (« if tag … »,
      « if health >= 50 ») ; la clause sélectionnée déplie SES champs
      seulement (tag / attribut+valeur / item picker+count / lua) +
      negate.
- [ ] `+ condition` pré-remplit le parent ; `x` supprime les drafts de
      session seulement (marqueur `o` sur les records base).
- [ ] Sélectionner une clause ne fait PAS disparaître la liste (ancrée
      sur le parent).

## 9. FX + preview (8.10) — 5 min

Catégorie **Particles** :
- [ ] La preview joue en boucle (burst et/ou rate), disques sur fond
      sombre, ligne de sol ; Restart / pause / time / zoom.
- [ ] Éditer `colorStart`, `rate`, `gravity` dans l'inspecteur → l'effet
      est **visible immédiatement**.
- [ ] Catégorie **Cues** : `particles`/`sound` en pickers ; « Used by »
      depuis le résumé central.
- [ ] Rappel assumé : pas d'additif réel ni de textures dans la preview
      (écrit dans le panneau) — la couleur finale se juge en jeu.

## 10. Effects & Abilities (8.11) — 10 min

Catégorie **Effects** → un effect existant (ou « poison ») :
- [ ] Sections contextuelles : `instant` ne montre aucun champ de durée ;
      `periodic` montre `period` + durées ; Expiry n'apparaît que si
      l'attribut est onyx/amber/garnet.
- [ ] Warnings : `periodic` avec period 0 → « never ticks » ;
      buildup + attribute → exclusivité signalée.
- [ ] **Test apply** : Reset actor → Apply → la table base/current
      bouge (un damage → health baisse ; un buff duration → current
      seulement ; « REFUSED » si requiredTag manque).

Catégorie **Abilities** → une ability :
- [ ] Wiring : cost/cooldown/effect résolus par nom, warnings sur guid
      dangling et **cooldown sans grantedTag**.
- [ ] **Test activate** : l'énergie paie le coût, le tag de cooldown se
      pose (2e activation → REFUSED), l'effet s'applique en self-cast.
      (Résout la base RÉSOLUE : exporter + recharger pour tester des
      effects créés en session — écrit dans le panneau.)

## Rappels transverses

- **Export** : UN plugin porte les éditions de tous les panneaux (la
  même session) ; jamais de x/y de nœuds dedans.
- **Undo** (Ctrl+Z) doit défaire proprement la dernière opération de
  n'importe quel panneau.
- Un record **base** ne se supprime jamais (§5) ; seuls les drafts créés
  en session partent.
- `data/editor-layouts.toml` = positions des nœuds (état d'outil, pas un
  plugin) ; le supprimer = retour à l'auto-layout, sans conséquence.
