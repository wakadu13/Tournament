# EMA Tournament — FPS multijoueur (Team Deathmatch)

FPS multijoueur en C avec [raylib](https://www.raylib.com/) : deux équipes
(rouge vs bleue) s'affrontent dans un match à mort par équipe. Première
équipe à **30 kills** gagne.

Les joueurs humains incarnent des **soldats-mercenaires** (treillis olive,
visage peint en vert, yeux injectés de sang, démarche bras tendus) ; les
bots sont des **poussins zombies** humanoïdes ensanglantés, tout droit
sortis d'un film d'horreur. Épaulettes, brassards et colliers prennent la
couleur de l'équipe.

## Architecture

```
src/
├── common/           Code partagé par les trois programmes
│   ├── protocol.h    Protocole réseau UDP (paquets, constantes)
│   └── game.h/.c     Règles, map (AABB), collisions, raycasts
├── server/main.c     Serveur dédié headless (autoritaire)
├── client/main.c     Client raylib (vue FPS, prédiction, HUD)
├── client/models.c   Modèles 3D procéduraux + animation (voir plus bas)
└── bot/main.c        Bot zombie headless (pour le dev et le test)
```

- **Transport** : UDP, paquets binaires packés (`#pragma pack`).
- **Serveur autoritaire** sur les tirs (raycast serveur), la vie, les
  kills, les scores et les respawns. Il valide l'expéditeur de chaque
  paquet (id + adresse), l'origine des tirs, et rejette les positions
  datant d'avant un respawn (compteur `spawn_count`).
- **Client prédictif** : le déplacement est simulé localement (fluide
  quel que soit le ping) et envoyé 30×/s ; les autres joueurs sont
  interpolés entre les snapshots (20 Hz).

## Vue FPS

- Arme dessinée en « view model » : passe 3D séparée en espace vue, elle
  ne rentre jamais dans les murs. Balancement de marche, sway (l'arme
  traîne derrière la souris), recul (kick) et flash de bouche.
- Visée (clic droit) : zoom de FOV, sensibilité réduite, point rouge du
  viseur au centre de l'écran, dispersion minimale.
- Sprint (Maj + avant) : FOV élargi, arme baissée, tir impossible.
- Dispersion réaliste : précise en visant, imprécise en courant, en
  sautant ou en rafale (bloom) — le réticule s'écarte pour la montrer.
- Head bob à la marche, caméra qui s'effondre à la mort, recul qui fait
  réellement monter la vue (et les balles).

## Modèles 3D et animation

Les modèles **et leur outillage** sont repris du premier jeu
(*ZEFFALIE COME BACK*) : tout est assemblé à partir de **trois maillages**
seulement — un cube, une sphère, un cylindre — placés par des matrices.
Rien n'est chargé depuis un fichier.

`Ball`, `Box3`, `Seg`, `Limb`, `BallAxes` et `TRS` prennent une matrice
`parent`, avec la convention `MatrixMultiply(local, parent)` = « local puis
parent ». Chaque personnage est donc un **arbre de repères** : le bec est
décrit dans le repère de la mâchoire, la mâchoire dans celui de la tête, la
tête dans celui du tronc. Quand la tête tourne, le bec suit tout seul — ce
sont de vraies articulations, pas des pièces déplacées une par une.

```
stance ......... le sol sous lui : porte les pattes, encaisse la chute
  body ......... tronc : dandinement, compression du pas, bond, respiration
    head ....... pivot du cou : tic d'épouvante, regard, plongeon
      hinge .... charnière du bec
        upper/lower .. les deux mandibules (le bec CLAQUE)
          drool ...... la bave qui s'allonge sous la mandibule basse
    wing × 2 ... charnière d'épaule : battement (axe X) + écartement (axe Y)
    tail ....... pivot de la queue : elle bat
```

### Ce qui bouge sur un poussin zombie

Tout l'état d'animation tient dans un `CharAnim` par joueur
(`src/client/models.h`). `CharAnimUpdate` le fait avancer à partir du **seul
déplacement affiché** : la vitesse horizontale donne l'allure, la verticale
le saut, et le serveur n'envoie **aucune donnée d'animation**.

| Couche | Ce qu'on voit |
|---|---|
| Marche / course | balancement des pattes, contre-balancement des ailes ; la foulée s'allonge et le corps se penche à mesure qu'il accélère |
| Dandinement | roulis du tronc, une fois par deux pas |
| Compression | le tronc s'écrase un peu à chaque appui |
| Respiration | le tronc se gonfle, surtout à l'arrêt |
| Repos | léger transfert de poids d'une patte sur l'autre |
| Tic d'épouvante | la tête penche lentement puis se redresse d'un coup |
| Regard | le cou suit le joueur ennemi vivant le plus proche (jusqu'à 20 m) |
| Clignement | une paupière descend sur l'orbite, à intervalles irréguliers |
| Ébrouement | il secoue ses plumes de la tête à la queue, houppette hérissée |
| Bec | entrouvert au repos, il claque des mandibules, s'ouvre en grand quand il charge |
| Bave | un filet s'allonge sous le bec, se détache et tombe |
| Griffade | bond en avant, bec grand ouvert qui **claque** au contact, ailes jetées devant, doigts crispés, lueur des yeux qui monte |
| Sursaut | il part en arrière quand il encaisse une balle |
| Saut | pattes repliées, ailes qui battent vite, atterrissage amorti |
| Mort | il bascule sur le flanc, les pattes se dérobent, la tête retombe, les paupières se ferment — et le corps **reste au sol** jusqu'au respawn |

Les tics sont désynchronisés par joueur (`CharAnimInit` décale l'horloge
d'après le numéro de slot) : deux poussins ne clignent jamais des yeux
ensemble. Et dès que la mort commence, toutes les animations de vivant
s'éteignent : il ne reste que l'effondrement.

Le **soldat** suit le même système, en plus sobre : marche, respiration à
l'arrêt, bouche qui s'ouvre à la course, sursaut, clignement et chute.

### L'enseigne

Le **poussin géant** du premier jeu tourne au bout d'un mât planté sur le
bloc central de l'arène, assez haut pour passer au-dessus d'un joueur monté
dessus : c'est le même `DrawChick` (25 cm, mis à l'échelle ×6), porté avec
sa signature d'origine pour rester interchangeable avec le premier jeu. Il
picore, pivote la tête entre deux coups de bec, cligne des yeux, s'ébroue,
s'étire une aile, gratte du pied et bat de la queue.

## Compilation (macOS)

```sh
brew install raylib
make
```

Produit `build/server`, `build/client` et `build/bot`. Le serveur et le
bot ne dépendent pas de raylib (ils tournent sans affichage).

## Lancer une partie

```sh
# Terminal 1 : le serveur (port 7777 par défaut)
./build/server

# Terminal 2 : votre client
./build/client 127.0.0.1 Alice

# Terminal 3 : un bot zombie pour avoir un adversaire (dev/test)
./build/bot 127.0.0.1 Zombie
```

Pour jouer en LAN, remplacer `127.0.0.1` par l'IP de la machine serveur.
L'équipe est attribuée automatiquement (auto-équilibrage) : connectez le
bot **après** vous pour qu'il soit dans l'équipe adverse.

### Le bot zombie

Headless, il chasse l'ennemi vivant le plus proche, contourne (grossièrement)
les obstacles en sautant et en faisant des pas de côté quand il est coincé,
et griffe au corps-à-corps (25 dégâts toutes les 0,9 s). Ses réglages sont en
tête de [src/bot/main.c](src/bot/main.c).

## Contrôles

| Touche | Action |
|---|---|
| W / A / S / D | Se déplacer (position physique des touches : ZQSD en AZERTY) |
| Souris | Viser |
| Clic gauche | Tirer (automatique maintenu) |
| Clic droit | Viser (zoom) |
| Maj gauche | Sprint (en avançant) |
| Espace | Sauter |
| Tab | Tableau des scores |
| Échap | Quitter |

## Réglages de gameplay

Tout est dans [src/common/game.h](src/common/game.h) : dégâts, cadence,
vitesse, gravité, délai de respawn, limite de score... et la map elle-même
dans [src/common/game.c](src/common/game.c) (liste de boîtes `MAP_BOXES`).
Les réglages de la vue FPS (FOV, sensibilité, accélérations) sont en tête
de [src/client/main.c](src/client/main.c).

## Pistes pour la suite

- Lag compensation (rembobiner les positions pour valider les tirs)
- Anti-triche sur le déplacement (valider la vitesse côté serveur)
- Plusieurs armes, munitions, rechargement
- Sons (tirs, hits, cris de poussin)
- Chat texte, menu de connexion in-game
