# EMA Tournament — FPS multijoueur (Team Deathmatch)

FPS multijoueur en C avec [raylib](https://www.raylib.com/) : deux équipes
(rouge vs bleue) s'affrontent par **manches à élimination**, comme dans
Counter-Strike, sur une carte de **120 × 120 m inspirée de Dust 2**, avec
**neuf armes** (FAMAS, HK416, M4A1, AK-74M, MP5, FR-F2, Benelli M3, Glock 17,
PAMAS G1) à choisir dans le **menu d'équipement**, et un **menu d'options**
(sensibilité, champ de vision, plein écran...) gardé d'une partie à l'autre.

## Le mode de jeu

- Une manche commence avec tout le monde vivant à son spawn. **Pas de
  respawn** : un membre éliminé le reste jusqu'à la fin de la manche (son
  corps reste au sol).
- L'équipe qui **n'a plus aucun survivant perd la manche**. Si les deux
  derniers tombent en même temps, la manche est nulle.
- Une manche dure au plus **2 minutes** (`ROUND_TIME`) : au temps, l'équipe
  qui a le plus de survivants l'emporte ; à égalité, manche nulle.
- Le match se joue en **5 manches gagnantes** (`ROUNDS_TO_WIN`), avec 4 s de
  pause entre deux manches.
- Un joueur qui arrive **en cours de manche** attend la suivante, éliminé
  d'office. S'il manque une équipe, le serveur attend.

Le HUD affiche les manches gagnées de chaque côté, le numéro de manche et
le chrono au centre, et un point par survivant sous chaque score.

## La carte

Inspirée de Dust 2, à l'échelle d'une vraie carte de Counter-Strike :
**120 × 120 m**. Elle est **construite au démarrage** (`MapInit` dans
[src/common/game.c](src/common/game.c)) par des fonctions qui posent des
murs percés de **portes** et de **fenêtres**, des **escaliers**, des
caisses, des tonneaux, des sacs de sable, des colonnes et des arcades :
environ 170 volumes qui bloquent joueurs et balles, et 200 éléments de
décor (battants de portes, encadrements, auvents, tapis, poutres,
créneaux) que seul le client dessine. Les T (bleus) partent au sud, les
CT (rouges) au nord, et trois voies relient les deux camps vers les deux
**sites** (A au nord-est, B au nord-ouest) :

| Voie | Description |
|---|---|
| **Mid** | couloir central de 20 m, coupé par les « doubles portes » (un mur percé d'un passage de 5 m, battants ouverts) ; l'enseigne au poussin, la caisse « xbox » et deux murets servent de couverture ; au nord, top mid et ses sacs de sable débouchent sur le spawn CT, abrité par une **arcade** à colonnes |
| **Catwalk** | une **passerelle à 2,40 m** : on monte un escalier depuis top mid, on longe le bloc nord jusqu'au site A, que l'on domine d'un garde-corps avant de redescendre par un second escalier |
| **Long A** | à l'est, un couloir de 12 m de large et 58 m de long, fermé au sud par les **long doors** (un passage couvert avec une porte à chaque bout), avec un muret, des sacs de sable et le « coin de long » et ses tonneaux ; il débouche sous une arche sur le **site A** |
| **Tunnels** | à l'ouest, les tunnels **hauts** longent l'enceinte sous un toit porté par des colonnes (il laisse passer un joueur mais bloque les tirs en cloche) jusqu'au **site B** ; les tunnels **bas** les relient à mid à travers le bloc ouest, sous leurs poutres |
| **Connecteur** | du haut de mid vers le site B, un couloir de 6 m ; il dessert la **salle de la fenêtre**, une pièce qui domine mid par une fenêtre : on tire à travers, on ne passe pas |
| **B doors** | du spawn CT au site B, une porte de 4 m avec son linteau |

Les sites ont un **plateau** à 1,60 m qui se monte par ses **escaliers**
(ou d'un saut, 1 m), des caisses empilées, des tonneaux et des sacs de
sable ; le site B a un **réduit** (une petite pièce contre l'enceinte,
porte à l'est), le spawn T une **maison** (porte à l'ouest, fenêtre au
nord) et son arcade. Toutes les voies font au moins 4 m de large.

Les **marches** de 0,55 m au plus se franchissent en marchant
(`STEP_HEIGHT`, dans la physique partagée : le client prédit exactement ce
que le serveur calcule) ; une caisse d'un mètre se saute.

Les bots suivent des **points de passage** (`NAV_NODES`, 26 nœuds reliés
quand on arrive de l'un à l'autre en marchant droit, escaliers compris) :
un zombie qui ne voit pas sa proie cherche le chemin le plus court sur ce
graphe et le suit, puis fonce dès qu'il l'aperçoit. Un harnais fait
**marcher un joueur simulé** sur chaque lien et vérifie que les spawns
sont dégagés, que le graphe est connexe, que les escaliers se montent,
que les portes se passent et que les fenêtres arrêtent les joueurs mais
pas les balles.

Les joueurs humains incarnent des **soldats-mercenaires** (treillis olive,
visage peint en vert, yeux injectés de sang, démarche bras tendus) ; les
bots sont des **poussins zombies** humanoïdes ensanglantés, tout droit
sortis d'un film d'horreur. Épaulettes, brassards et colliers prennent la
couleur de l'équipe.

## Les armes

Neuf armes d'armée conventionnelle, décrites dans la table `WEAPONS` de
[src/common/game.c](src/common/game.c) (partagée : le serveur applique
dégâts, cadence, portée, chargeur et plombs ; le client s'en sert pour la
dispersion, le recul, le HUD et le menu). Chaque joueur part avec une
**arme principale** et une **arme de poing** (touches 1 / 2, molette, ou le
bouton d'arme sur Android) :

| Arme | Catégorie | Tir | Dégâts | Cadence | Portée | Chargeur |
|---|---|---|---|---|---|---|
| **FAMAS F1** | fusil d'assaut (bullpup) | automatique | 22 | 1000 cpm | 150 m | 25 + 75 |
| **HK416** | fusil d'assaut | automatique | 26 | 850 cpm | 160 m | 30 + 90 |
| **M4A1** | carabine | automatique | 25 | 800 cpm | 150 m | 30 + 90 |
| **AK-74M** | fusil d'assaut | automatique | 31 | 650 cpm | 150 m | 30 + 90 |
| **HK MP5** | pistolet-mitrailleur | automatique | 18 | 800 cpm | 60 m | 30 + 120 |
| **FR-F2** | fusil de précision | à verrou, lunette | 100 | 40 cpm | 300 m | 10 + 30 |
| **Benelli M3** | fusil à pompe | à pompe | 8 × 12 | 67 cpm | 25 m | 7 + 28 |
| **Glock 17** | pistolet (poing) | coup par coup | 20 | 500 cpm | 60 m | 17 + 51 |
| **PAMAS G1** | pistolet (poing) | coup par coup | 24 | 430 cpm | 60 m | 15 + 45 |

- Un **tir à la tête** (dans les 30 cm du haut) fait **×2**. Les fusils
  d'assaut tuent en 4 balles au corps, le FR-F2 d'une seule.
- **Rechargement** (R) : la réserve remplit le chargeur, en 1,6 à 3,2 s
  selon l'arme ; à vide, l'arme recharge seule (option désactivable).
  **Changement d'arme** : 0,45 s pendant lesquelles on ne tire pas.
- L'**équipement** se choisit dans le menu (B, ou le menu pause) : comme
  dans Counter-Strike, il s'applique **tout de suite pendant les 20
  premières secondes d'une manche** (`ROUND_BUY_TIME`), quand on est mort
  ou entre deux manches ; sinon il attend la prochaine manche.
- Le serveur **tient les chargeurs** : un client modifié ne tire ni plus
  vite, ni plus fort, ni sans munitions, ni avec l'arme qu'il n'a pas en
  main. Chaque tir porte un numéro que le serveur accuse dans le snapshot
  (`last_shot_seq`) : le client affiche ses munitions moins ses tirs
  encore en route, sans jamais « sauter ».
- Le fusil à pompe tire huit plombs par coup, dispersés par le serveur ;
  l'arme pèse sur la vitesse (le MP5 et les pistolets rendent plus
  rapide, le FR-F2 plus lent), ce que le client et le serveur simulent
  à l'identique.

Les bots (poussins zombies) n'ont que leurs **griffes** : 25 dégâts à
2,5 m, sans munitions.

## Architecture

```
src/
├── common/           Code partagé par les trois programmes
│   ├── protocol.h    Protocole réseau UDP (paquets, constantes)
│   └── game.h/.c     Règles, armes, construction de la map (AABB), points de passage, collisions
│   └── platform.h    Sockets, horloge et sleep : Windows / Linux / macOS / Android
├── server/main.c     Serveur dédié headless (autoritaire)
├── client/main.c     Client raylib (vue FPS, prédiction, HUD, boucle des menus)
├── client/models.c   Modèles 3D procéduraux + animation (voir plus bas)
├── client/weapons.c  Modèles 3D des neuf armes (vue subjective et dans les mains des soldats)
├── client/menu.c     Accueil, pause, équipement, options
├── client/ui.c       Widgets immédiats (boutons, curseurs, champs, clavier à l'écran)
├── client/options.c  Réglages persistants (options.cfg)
├── client/touch.c    Commandes tactiles (Android)
└── bot/main.c        Bot zombie headless (pour le dev et le test)

android/              Manifeste, icône et clé de signature de l'APK
scripts/              build_android.sh (APK sans Gradle)
.github/workflows/    CI : compile les 4 plateformes et publie la release
```

- **Transport** : UDP, paquets binaires packés (`#pragma pack`).
- **Serveur autoritaire sur tout** : le déplacement, les tirs, la vie,
  les kills, les scores et les respawns. Le client n'envoie jamais de
  position, seulement ses **entrées** (déplacement, vue, saut, sprint,
  visée), que le serveur rejoue avec le même code de simulation que lui
  (`PlayerStep`, `game.c`). Un client modifié peut envoyer n'importe
  quelles entrées ; il ne peut ni se téléporter ni accélérer (budget de
  temps simulé borné par l'horloge du serveur). Il valide aussi
  l'expéditeur de chaque paquet (id + adresse), l'origine des tirs, et
  rejette les entrées datant d'avant un respawn (compteur `spawn_count`).
- **Client prédictif et réconcilié** : chaque image, le client simule son
  entrée tout de suite (fluide quel que soit le ping) et la garde jusqu'à
  ce que le serveur l'acquitte (`last_seq` du snapshot). À chaque
  snapshot il repart de l'état que le serveur lui donne et rejoue ses
  entrées plus récentes : si ses prédictions étaient justes, il retombe
  au même endroit ; sinon la correction s'applique. Les 8 dernières
  entrées partent dans chaque paquet (30×/s) : une perte UDP ne coûte
  rien.
- **Compensation de lag** : les autres joueurs sont affichés 100 ms dans
  le passé, interpolés entre les deux snapshots (20 Hz) qui encadrent cet
  instant, et le client joint à chaque tir **l'instant qu'il voyait**. Le
  serveur garde une seconde d'historique des positions et **rembobine les
  cibles à cet instant** avant de lancer le rayon : on touche ce qu'on
  visait, pas là où la cible est arrivée pendant le trajet du paquet.

## Vue FPS

- Arme dessinée en « view model » : passe 3D séparée en espace vue, elle
  ne rentre jamais dans les murs. Balancement de marche, sway (l'arme
  traîne derrière la souris), recul (kick) et flash de bouche, et les
  animations de rechargement (l'arme bascule, le chargeur sort),
  de changement d'arme (elle descend puis remonte) et de réarmement
  (verrou du FR-F2, pompe du Benelli, glissière des pistolets).
- Visée (clic droit) : zoom de FOV, sensibilité réduite, point rouge du
  viseur au centre de l'écran, dispersion minimale. Chaque arme a sa
  ligne de visée à la même hauteur dans son repère (`WEAPON_SIGHT_Y`) :
  le viseur tombe au centre quelle que soit l'arme. Le FR-F2 vise dans
  une **lunette** plein écran (FOV 12°).
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

### Un seul appel de dessin par image

Chaque `Ball` / `Box3` / `Seg` était un `DrawModel` à part : une
soixantaine par poussin, un millier par image à 16 joueurs. Sur un
téléphone en OpenGL ES 2, c'est l'appel de dessin qui coûte, pas le
triangle — et l'instanciation n'y existe pas. Entre `BatchBegin()` et
`BatchEnd()`, `DrawPart` transforme sur le CPU les sommets des trois
maillages unitaires et les ajoute à un maillage dynamique, envoyé et
dessiné **une fois par image** : tous les personnages et l'enseigne tiennent
dans un appel.

Comme le CPU tient les sommets, l'ombrage est **cuit dans la couleur** de
chacun (70 % d'ambiance, 30 % selon l'angle entre la normale et la lumière)
: du relief là où tout était plat, sans shader, pour un coût nul au rendu.
Hors d'un lot, les primitives se dessinent une à une comme avant.

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

## Téléchargements

Chaque push sur `main` republie la release **« latest »** avec les
exécutables des quatre plateformes, compilés par la CI
([.github/workflows/build.yml](.github/workflows/build.yml)) :

| Fichier | Contenu |
|---|---|
| `EMATournament-windows-x64.zip` | le jeu, le serveur et le bot (`.exe` autonomes, aucune DLL à installer) |
| `EMATournament-macos-arm64.tar.gz` | `client`, `server`, `bot` (Apple Silicon) |
| `EMATournament-linux-x64.tar.gz` | `client`, `server`, `bot` |
| `EMATournament-android.apk` | le jeu au doigt (Android 7.0+, `arm64-v8a` et `x86_64`) |

Avant d'être publié, **l'APK est lancé dans un émulateur Android** par la
CI (`scripts/android_smoke_test.sh`) : le jeu doit démarrer, initialiser
l'affichage, puis **se connecter en UDP à un serveur qui tourne sur la
machine hôte** — l'adresse est tapée sur le clavier à l'écran du menu
d'accueil, comme le ferait un joueur. Les captures d'écran de chaque étape
sont dans les artefacts du run (`captures-android`). Un APK qui ne démarre
pas, ou qui ne joint pas le serveur, bloque la release.

L'APK est signé avec la clé de **debug** du dépôt
(`android/debug.keystore`) : Android demandera d'autoriser une source
inconnue à l'installation, et les mises à jour s'installeront par-dessus
la version précédente.

## Compilation

Le code n'utilise que le C11 standard plus la couche
[src/common/platform.h](src/common/platform.h), qui masque les différences
de sockets et d'horloge entre les systèmes. Il n'y a rien à adapter.

### macOS

```sh
brew install raylib
make
```

### Linux

```sh
sudo apt-get install -y build-essential libraylib-dev
make
```

Si la distribution ne fournit pas raylib, on la compile depuis ses sources
et on indique son chemin :

```sh
git clone --depth 1 --branch 5.5 https://github.com/raysan5/raylib.git raylib-src
make -C raylib-src/src PLATFORM=PLATFORM_DESKTOP -j 4
make RAYLIB_PATH=raylib-src
```

### Windows

Par cross-compilation depuis Linux (c'est ce que fait la CI), avec raylib
compilée pour Windows :

```sh
sudo apt-get install -y mingw-w64
git clone --depth 1 --branch 5.5 https://github.com/raysan5/raylib.git raylib-win
make -C raylib-win/src PLATFORM=PLATFORM_DESKTOP OS=Windows_NT \
     CC=x86_64-w64-mingw32-gcc AR=x86_64-w64-mingw32-ar -j 4
make windows RAYLIB_WIN_PATH=raylib-win
```

### Android

```sh
export ANDROID_HOME=~/Android/Sdk ANDROID_NDK=~/Android/Sdk/ndk/<version>
git clone --depth 1 --branch 5.5 https://github.com/raysan5/raylib.git raylib-src
RAYLIB_SRC="$PWD/raylib-src" bash scripts/build_android.sh
```

Pas de Gradle : le script compile raylib et le client en une bibliothèque
`libmain.so` par architecture, puis empaquette avec `aapt`, `zipalign` et
`apksigner` (voir [scripts/build_android.sh](scripts/build_android.sh)).

`make` produit `build/server`, `build/client` et `build/bot`. **Le serveur
et le bot ne dépendent pas de raylib** : `make server` suffit sur une
machine sans affichage.

## Lancer une partie

```sh
# Terminal 1 : le serveur (port 7777 par défaut)
./build/server

# Terminal 2 : votre client
./build/client 127.0.0.1 Alice

# Terminal 3 : un bot zombie pour avoir un adversaire (dev/test)
./build/bot 127.0.0.1 Zombie
```

Le client et le bot prennent `hôte[:port]` : une IP comme un **nom DNS**,
avec un port facultatif collé à l'adresse.

```sh
./build/client 192.168.1.20 Alice            # LAN, port 7777 par défaut
./build/client jeu.exemple.fr Alice          # par son nom DNS
./build/client jeu.exemple.fr:27015 Alice    # sur un autre port
```

L'équipe est attribuée automatiquement (auto-équilibrage) : connectez le
bot **après** vous pour qu'il soit dans l'équipe adverse.

### Le bot zombie

Headless, il chasse l'ennemi vivant le plus proche, contourne (grossièrement)
les obstacles en sautant et en faisant des pas de côté quand il est coincé,
et griffe au corps-à-corps (25 dégâts toutes les 0,9 s). Ses réglages sont en
tête de [src/bot/main.c](src/bot/main.c).

## Serveur dédié sur Linux

Le serveur est **headless** : il ne dépend ni de raylib, ni d'OpenGL, ni
d'un affichage. Il tient dans un binaire d'environ 20 Ko et tourne sur le
plus petit VPS venu.

### 1. Compiler

Aucune dépendance en dehors d'un compilateur C et de la libm :

```sh
sudo apt-get install -y build-essential git
git clone https://github.com/wakadu13/Tournament.git && cd Tournament
make server
```

Ou plus simplement : prendre `server` dans
`EMATournament-linux-x64.tar.gz` de la release « latest ».

```sh
./build/server          # port 7777 par défaut
./build/server 27015    # ou le port de votre choix
```

Le serveur affiche les connexions, les éliminations et le score sur sa
sortie standard. Il est **autoritaire** : c'est lui qui déplace les
joueurs à partir de leurs entrées, lui qui valide les tirs (en rembobinant
les cibles à l'instant que le tireur voyait), et il vérifie l'expéditeur
de chaque paquet (identifiant + adresse) et l'origine des tirs.

### 2. L'installer en service (systemd)

Pour qu'il démarre au boot et redémarre tout seul après un plantage :

```sh
# Un utilisateur sans privilèges, sans shell de connexion
sudo useradd --system --no-create-home --shell /usr/sbin/nologin ematour
sudo install -m 755 build/server /usr/local/bin/ematournament-server
```

`/etc/systemd/system/ematournament.service` :

```ini
[Unit]
Description=Serveur EMA Tournament (UDP)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=ematour
Group=nogroup
ExecStart=/usr/local/bin/ematournament-server 7777
Restart=always
RestartSec=2

# Le serveur n'a besoin de rien d'autre que d'une socket UDP :
# on lui retire tout le reste.
NoNewPrivileges=true
PrivateTmp=true
PrivateDevices=true
ProtectSystem=strict
ProtectHome=true
ProtectKernelTunables=true
ProtectControlGroups=true
RestrictAddressFamilies=AF_INET
RestrictNamespaces=true
MemoryMax=128M

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now ematournament
sudo systemctl status ematournament
journalctl -u ematournament -f      # suivre les connexions en direct
```

### 3. Ouvrir le port

Le jeu parle **UDP**, et uniquement UDP. C'est l'erreur classique :
n'ouvrez pas le port en TCP, rien ne passerait.

```sh
sudo ufw allow 7777/udp
```

Chez un hébergeur (AWS, GCP, OVH, Hetzner…), il faut **aussi** autoriser
`UDP 7777` en entrée dans le groupe de sécurité / pare-feu du panneau
d'administration : le pare-feu de la machine ne suffit pas.

Vérification depuis un autre poste :

```sh
./client ADRESSE_DU_SERVEUR Alice      # ou ADRESSE:PORT si ce n'est pas 7777
# ou, sans le jeu :
nc -u -z -v ADRESSE_DU_SERVEUR 7777
```

### 4. Ce que ça consomme

Mesuré sur le protocole réel (`src/common/protocol.h`) :

| | Valeur |
|---|---|
| Taille d'un snapshot | **1 266 octets** (16 emplacements de 78 octets, armes et munitions comprises, + en-tête) |
| Fréquence d'envoi | 20 Hz (`SNAPSHOT_HZ`) |
| Montant serveur, 16 joueurs | 1 266 × 20 × 16 ≈ **405 ko/s, soit 3,2 Mbit/s** |
| Montant par client | 148 octets (8 entrées) × 30 Hz ≈ **4,4 ko/s** |
| Tick de simulation | 60 Hz, un seul fil d'exécution ; les entrées sont rejouées à leur arrivée |
| Historique pour les tirs | 64 ticks ≈ 1 s de positions, pour tous les joueurs |

Autrement dit : un VPS à 1 vCPU / 512 Mo et 3 Mbit/s d'upload suffit pour
une partie pleine. Le serveur envoie l'état **complet** du monde à chaque
snapshot (pas de delta), donc la bande passante monte au **carré** du
nombre de joueurs — c'est ce qui fixe `MAX_PLAYERS` à 16.

## Et avec Cloudflare ?

**À lire avant de s'y lancer : le proxy de Cloudflare — le nuage orange —
ne transporte que du HTTP(S). Il ne peut pas servir de façade à un port de
jeu en UDP.** Activer le proxy sur l'enregistrement DNS du serveur ne
ferait pas « passer » le jeu : cela l'empêcherait de fonctionner. Trois
chemins existent, du plus simple au plus lourd.

### Option 1 — Cloudflare en DNS seul (ce qui marche aujourd'hui)

C'est la configuration à utiliser par défaut. Cloudflare sert de DNS, le
trafic de jeu va directement au VPS.

1. Dans **DNS → Records**, ajouter un enregistrement `A` :
   `jeu.exemple.fr` → l'IP du VPS.
2. **Désactiver le proxy** : le nuage doit être **gris** (« DNS only »),
   pas orange.
3. Les joueurs se connectent avec `./client jeu.exemple.fr Alice` — le
   client résout les noms DNS (`getaddrinfo`), il n'y a donc pas d'IP à
   retenir.

Avantages : gratuit, immédiat, et l'adresse reste lisible même si l'IP du
serveur change. Limites : l'IP du serveur est publique et le port 7777
n'est protégé par rien — c'est le pare-feu du VPS qui fait le travail.

### Option 2 — Cloudflare Spectrum (le seul moyen de proxifier l'UDP)

Spectrum proxifie des protocoles autres que HTTP, et c'est le **seul**
produit Cloudflare capable de relayer de l'UDP. L'IP du serveur reste
cachée et le trafic bénéficie de la protection anti-DDoS.

Deux réserves, importantes :

- **L'UDP via Spectrum est réservé au plan Enterprise.** Les plans Pro et
  Business ne proposent qu'une liste de protocoles TCP. Ce n'est donc pas
  une option pour une partie entre amis.
- Le trafic Spectrum est facturé au volume.

Configuration, le cas échéant : **Spectrum → Create an application**,
protocole UDP, port d'écoute `7777`, origine `IP_DU_VPS:7777`. Côté jeu il
n'y a rien à changer : les clients visent le nom Cloudflare au lieu de
l'IP du VPS.

### Option 3 — Faire tourner le serveur SUR Cloudflare (Workers / Durable Objects)

Ce serveur-ci **ne peut pas** être déployé tel quel sur Cloudflare
Workers : un Worker n'a pas de socket UDP en écoute. `bind()` sur un port
UDP n'existe pas dans ce runtime, et c'est précisément ce que fait
`src/server/main.c`.

Pour héberger la partie sur Cloudflare, il faudrait **changer de
transport** et porter la boucle serveur dans un **Durable Object** — un
objet unique par partie, qui sert justement de point de coordination pour
un salon ou un match, et qui sait parler WebSocket. Le travail à prévoir :

1. **Transport WebSocket côté client.** Remplacer `sendto`/`recvfrom` par
   un envoi de trames WebSocket. Les paquets binaires de
   [src/common/protocol.h](src/common/protocol.h) peuvent partir tels
   quels en trames binaires, donc le protocole lui-même ne bouge pas.
   Sur PC, cela veut dire lier une bibliothèque WebSocket ; pour une
   version navigateur, c'est l'API WebSocket native (c'est d'ailleurs
   l'approche de la version web du premier jeu, `src/netsock_web.c`).
2. **Boucle serveur dans un Durable Object.** Réécrire en JavaScript ou
   TypeScript la logique de `src/server/main.c` : le tick à 60 Hz devient
   une alarme du Durable Object, les snapshots partent à 20 Hz sur chaque
   WebSocket ouverte. Les collisions et les raycasts de
   [src/common/game.c](src/common/game.c) doivent être réimplémentés, ou
   compilés en WebAssembly pour rester **exactement** les mêmes des deux
   côtés — sinon le client prédit autre chose que le serveur.
3. **Conséquence sur la latence.** WebSocket, c'est du TCP : une perte de
   paquet bloque les suivants (« head-of-line blocking »). Pour un jeu de
   tir à 60 Hz, c'est un vrai recul par rapport à l'UDP. L'intérêt serait
   ailleurs : plus de VPS à administrer, et un jeu jouable directement
   depuis un navigateur.

En résumé : **option 1 pour jouer dès ce soir**, option 2 si vous avez un
contrat Enterprise, option 3 si vous voulez une version navigateur — et
c'est alors un chantier à part entière, pas un réglage.

Références : [ports réseau de
Cloudflare](https://developers.cloudflare.com/fundamentals/reference/network-ports/),
[prise en main de
Spectrum](https://developers.cloudflare.com/spectrum/get-started/),
[WebSockets dans
Workers](https://developers.cloudflare.com/workers/runtime-apis/websockets/),
[Durable Objects](https://www.cloudflare.com/products/durable-objects/).

## Menus et options

Le client s'ouvre sur un **écran d'accueil** : adresse du serveur
(`hôte[:port]`), pseudo, **JOUER**, **ARMES** (choix de l'équipement, avec
les caractéristiques et un aperçu 3D de chaque arme), **OPTIONS**,
**QUITTER**. En jeu, **Échap** ouvre le menu pause (reprendre, armes,
options, quitter la partie) et **B** ouvre directement l'équipement.

Les options : sensibilité souris, sensibilité au doigt (Android), champ
de vision (60 à 100°), inversion de l'axe vertical, plein écran (PC),
affichage des FPS, rechargement automatique. Avec le dernier serveur, le
pseudo et l'équipement choisi, elles sont écrites dans **`options.cfg`**
(texte `clé=valeur`), à côté de l'exécutable sur PC et dans le dossier
privé de l'application sur Android. Un fichier absent ou abîmé donne
simplement les valeurs par défaut.

Sur PC, `client <adresse[:port]> [pseudo]` saute l'accueil et se connecte
directement (pratique pour les scripts et les tests).

## Contrôles

| Touche | Action |
|---|---|
| W / A / S / D | Se déplacer (position physique des touches : ZQSD en AZERTY) |
| Souris | Viser |
| Clic gauche | Tirer (maintenu pour les armes automatiques, un clic par coup sinon) |
| Clic droit | Viser (zoom ; lunette avec le FR-F2) |
| R | Recharger |
| 1 / 2, molette | Arme principale / arme de poing |
| B | Choisir son équipement |
| Maj gauche | Sprint (en avançant) |
| Espace | Sauter |
| Tab | Tableau des scores |
| Échap | Menu pause |

### Sur Android

Le téléphone n'a ni clavier ni ligne de commande, donc le client change sur
deux points (tout est derrière `MOBILE_UI` dans
[src/client/main.c](src/client/main.c) et `TOUCH_ENABLED` dans
[src/client/touch.c](src/client/touch.c)) :

- **Écran d'accueil** : l'adresse du serveur et le pseudo se tapent sur un
  **clavier à l'écran** (chiffres, AZERTY, point, deux-points, tiret,
  effacement ; le `:` permet de viser un autre port, par ex.
  `192.168.1.20:27015`), puis **JOUER**. Les menus ARMES et OPTIONS sont
  les mêmes que sur PC.
- **Commandes tactiles**, reprises du premier jeu :
  - 40 % gauche de l'écran : **joystick virtuel**, il apparaît sous le doigt
    là où on se pose ; poussé à fond vers l'avant il déclenche le **sprint** ;
  - partout ailleurs : **glisser pour viser** ;
  - boutons ronds en bas à droite : **TIRER** (on peut viser en glissant
    depuis ce bouton, donc tirer et viser du même pouce), **VISER**
    (bascule), **SAUT**, **RECH.** (recharger) et le bouton d'arme, qui
    affiche les munitions de l'arme en main et **change d'arme** ;
  - en haut à gauche : **MENU** (pause, armes, options).

  Chaque doigt reçoit un rôle au moment où il se pose et le garde jusqu'à ce
  qu'il soit levé : on peut se déplacer, viser et tirer à trois doigts.

Le jeu prend **tout l'écran** (`InitWindow(0, 0)` : raylib adopte la taille
native, sans bande noire). Le HUD est dessiné en **unités d'interface**
(720 de haut, largeur selon le ratio de l'écran) et mis à l'échelle d'un
coup par `BeginMode2D` : il garde la même taille relative d'un téléphone à
une tablette.

Pour régler tout ça **sans téléphone**, compilez avec
`-DTOUCH_CONTROLS=1` : le clavier à l'écran et les commandes tactiles
s'activent sur PC, la souris jouant le rôle du doigt.

## Réglages de gameplay

Tout est dans [src/common/game.h](src/common/game.h) : vitesse, gravité,
durée d'une manche, manches gagnantes, temps d'achat, multiplicateur des
tirs à la tête, hauteur de marche... La map (`MapInit` : murs, portes,
fenêtres, escaliers, caisses...), les points de passage des bots
(`NAV_NODES`) et la table des armes (`WEAPONS` : dégâts, cadence, portée,
chargeur, dispersion, recul) sont dans
[src/common/game.c](src/common/game.c).
Les réglages de la vue FPS (FOV, sensibilité, accélérations) sont en tête
de [src/client/main.c](src/client/main.c).

## Pistes pour la suite

- Sons (tirs, hits, cris de poussin)
- Chat texte
- Grenades, et un vrai pathfinding pour les bots (aujourd'hui : points de passage)
- Delta-compression des snapshots, pour dépasser 16 joueurs
- Signature de release de l'APK (aujourd'hui signé avec la clé de debug)
