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
│   └── platform.h    Sockets, horloge et sleep : Windows / Linux / macOS / Android
├── server/main.c     Serveur dédié headless (autoritaire)
├── client/main.c     Client raylib (vue FPS, prédiction, HUD)
├── client/models.c   Modèles 3D procéduraux + animation (voir plus bas)
├── client/touch.c    Commandes tactiles (Android)
└── bot/main.c        Bot zombie headless (pour le dev et le test)

android/              Manifeste, icône et clé de signature de l'APK
scripts/              build_android.sh (APK sans Gradle)
.github/workflows/    CI : compile les 4 plateformes et publie la release
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
machine hôte** — l'adresse est tapée sur le pavé numérique de l'écran
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
sortie standard. Il est **autoritaire** : il valide l'expéditeur de chaque
paquet (identifiant + adresse), l'origine des tirs, et rejette les
positions datant d'avant un respawn.

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
| Taille d'un snapshot | **779 octets** (16 emplacements de 48 octets + en-tête) |
| Fréquence d'envoi | 20 Hz (`SNAPSHOT_HZ`) |
| Montant serveur, 16 joueurs | 779 × 20 × 16 ≈ **250 ko/s, soit 2 Mbit/s** |
| Montant par client | 23 octets × 30 Hz ≈ **690 octets/s** |
| Tick de simulation | 60 Hz, un seul fil d'exécution |

Autrement dit : un VPS à 1 vCPU / 512 Mo et 2 Mbit/s d'upload suffit pour
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

### Sur Android

Le téléphone n'a ni clavier ni ligne de commande, donc le client change sur
deux points (tout est derrière `MOBILE_UI` dans
[src/client/main.c](src/client/main.c) et `TOUCH_ENABLED` dans
[src/client/touch.c](src/client/touch.c)) :

- **Écran d'accueil** : l'adresse du serveur se tape sur un pavé numérique
  (le `:` permet de viser un autre port, par ex. `192.168.1.20:27015`),
  puis **CONNECTER**. Sur PC l'adresse reste un argument de la ligne de
  commande.
- **Commandes tactiles**, reprises du premier jeu :
  - 40 % gauche de l'écran : **joystick virtuel**, il apparaît sous le doigt
    là où on se pose ; poussé à fond vers l'avant il déclenche le **sprint** ;
  - partout ailleurs : **glisser pour viser** ;
  - boutons ronds en bas à droite : **TIRER** (on peut viser en glissant
    depuis ce bouton, donc tirer et viser du même pouce), **VISER**
    (bascule) et **SAUT**.

  Chaque doigt reçoit un rôle au moment où il se pose et le garde jusqu'à ce
  qu'il soit levé : on peut se déplacer, viser et tirer à trois doigts.

Le jeu prend **tout l'écran** (`InitWindow(0, 0)` : raylib adopte la taille
native, sans bande noire). Le HUD est dessiné en **unités d'interface**
(720 de haut, largeur selon le ratio de l'écran) et mis à l'échelle d'un
coup par `BeginMode2D` : il garde la même taille relative d'un téléphone à
une tablette.

Pour régler tout ça **sans téléphone**, compilez avec
`-DTOUCH_CONTROLS=1` : l'écran d'accueil et les commandes tactiles
s'activent sur PC, la souris jouant le rôle du doigt.

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
- Chat texte, et menu de connexion in-game sur PC (il existe déjà sur Android)
- Delta-compression des snapshots, pour dépasser 16 joueurs
- Signature de release de l'APK (aujourd'hui signé avec la clé de debug)
