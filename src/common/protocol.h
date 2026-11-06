// protocol.h — Protocole réseau partagé client/serveur (UDP)
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

// v2 : le client envoie ses ENTREES (deplacement, vue, saut...), plus ses
// positions. Le serveur simule lui-meme : un client modifie ne peut plus se
// teleporter. Les tirs portent l'instant que le tireur VOYAIT, et le serveur
// rembobine les cibles a cet instant (compensation de lag).
// v3 : manches a elimination. Plus de respawn pendant une manche ; une
// equipe sans survivant perd la manche ; le snapshot porte la manche, sa
// phase et son chrono.
// v4 : plusieurs armes. Le joueur choisit un equipement (arme principale +
// arme de poing), change d'arme et recharge par PKT_CL_WEAPON ; le serveur
// tient les chargeurs et les reserves, applique degats, cadence, portee et
// plombs de l'arme ACTIVE, et relaie l'arme de chacun dans le snapshot.
// v5 : mode bombe. Un T porte la bombe, la pose sur un site (touche
// d'action, INPUT_USE), les CT la desamorcent ; le snapshot porte l'etat
// de la bombe. Les bots peuvent etre des SOLDATS armes (is_bot = 2). Le
// serveur diffuse chaque tir (EV_SHOT) pour que les clients l'entendent.
#define PROTO_VERSION   5
#define DEFAULT_PORT    7777

#define MAX_PLAYERS     16
#define NAME_MAX        16

// Entrees du joueur
#define INPUT_BATCH     8       // entrees par paquet : les dernieres, repetees d'un paquet a l'autre (pertes)
#define INPUT_MAX_DT    0.05f   // duree maximale simulee par une entree (s) : au-dela, triche ou gel
#define INPUT_JUMP      0x01
#define INPUT_SPRINT    0x02
#define INPUT_ADS       0x04
#define INPUT_USE       0x08    // action maintenue : poser / desamorcer la bombe

// Compensation de lag
#define HISTORY_TICKS   64      // positions gardees par le serveur (~1 s a 60 Hz)
#define INTERP_DELAY    0.10f   // le client affiche les autres 100 ms dans le passe (2 snapshots)

// Fréquences
#define SERVER_TICK_HZ  60      // simulation serveur
#define SNAPSHOT_HZ     20      // envoi des snapshots aux clients
#define CLIENT_SEND_HZ  30      // envoi des états client au serveur

// Timeout : un client/serveur silencieux pendant 5 s est déconnecté
#define NET_TIMEOUT_SEC 5.0

// Types de paquets
typedef enum {
    // Client -> Serveur
    PKT_CL_CONNECT = 1,     // demande de connexion (nom)
    PKT_CL_INPUT,           // entrees du joueur (deplacement, vue, saut...)
    PKT_CL_SHOOT,           // tir (origine + direction + instant vu)
    PKT_CL_DISCONNECT,      // départ propre
    PKT_CL_WEAPON,          // equipement, changement d'arme, rechargement

    // Serveur -> Client
    PKT_SV_ACCEPT = 100,    // connexion acceptée (id + équipe)
    PKT_SV_REJECT,          // serveur plein / version incompatible
    PKT_SV_SNAPSHOT,        // état complet du monde
    PKT_SV_EVENT,           // événement ponctuel (kill, hit, respawn...)
} PacketType;

typedef enum {
    TEAM_RED = 0,
    TEAM_BLUE = 1,
    TEAM_COUNT = 2,
} Team;

typedef enum {
    EV_KILL = 1,        // a = tueur, b = victime, value = arme (WeaponId), ou -1 si tir a la tete
    EV_HIT,             // a = tireur, b = touché, value = dégâts
    EV_RESPAWN,         // a = joueur
    EV_PLAYER_JOIN,     // a = joueur
    EV_PLAYER_LEAVE,    // a = joueur
    EV_MATCH_OVER,      // a = équipe gagnante
    EV_ROUND_START,     // a = numéro de manche
    EV_ROUND_END,       // a = équipe gagnante (TEAM_COUNT = manche nulle), b = numéro
    EV_LOADOUT,         // a = joueur, value = 1 si applique tout de suite, 0 si a la prochaine manche
    EV_SHOT,            // a = tireur, value = arme : un tir accepte par le serveur (pour le son)
    EV_BOMB_PLANTED,    // a = poseur, b = site (0 = A, 1 = B)
    EV_BOMB_DEFUSED,    // a = desamorceur
    EV_BOMB_EXPLODED,   // b = site
    EV_BOMB_DROPPED,    // a = joueur qui la portait
    EV_BOMB_PICKED,     // a = joueur qui la ramasse
} EventType;

// Pourquoi la manche s'est finie (value de EV_ROUND_END)
typedef enum {
    RE_ELIMINATION = 0, // plus de survivant dans une equipe
    RE_TIME,            // temps ecoule
    RE_EXPLODED,        // la bombe a explose
    RE_DEFUSED,         // la bombe a ete desamorcee
    RE_DRAW,            // nulle
} RoundEndReason;

// Etat de la bombe
typedef enum {
    BOMB_NONE = 0,      // pas de bombe cette manche (aucun T capable de la porter)
    BOMB_CARRIED,       // portee par bomb_carrier
    BOMB_DROPPED,       // au sol, a ramasser par un T
    BOMB_PLANTED,       // posee : bomb_timer compte a rebours
    BOMB_DEFUSED,
    BOMB_EXPLODED,
} BombState;

// Types de joueurs (PacketConnect.is_bot, relaye dans le snapshot)
typedef enum {
    BOT_NONE = 0,       // joueur humain
    BOT_ZOMBIE,         // poussin zombie : griffes, fonce sur sa proie
    BOT_SOLDIER,        // soldat arme : vise, tire, recharge, pose et desamorce
} BotKind;

// Phase de la manche en cours
typedef enum {
    ROUND_WAIT = 0,     // il manque une équipe : on attend
    ROUND_PLAY,         // manche en cours
    ROUND_END,          // manche finie, pause avant la suivante
} RoundPhase;

// --- Armes ----------------------------------------------------------------
// Les statistiques (degats, cadence, chargeur, dispersion...) sont dans la
// table WEAPONS de game.c, partagee par le serveur et le client.
typedef enum {
    WPN_FAMAS = 0,      // FAMAS F1 : fusil d'assaut bullpup, 5,56 mm
    WPN_HK416,          // HK416 : fusil d'assaut, 5,56 mm
    WPN_M4A1,           // M4A1 : carabine, 5,56 mm
    WPN_AK74,           // AK-74M : fusil d'assaut, 5,45 mm
    WPN_MP5,            // HK MP5 : pistolet-mitrailleur, 9 mm
    WPN_FRF2,           // FR-F2 : fusil de precision a verrou, 7,62 mm
    WPN_BENELLI,        // Benelli M3 : fusil a pompe, calibre 12
    WPN_GLOCK,          // Glock 17 : pistolet, 9 mm (arme de poing)
    WPN_PAMAS,          // PAMAS G1 : pistolet, 9 mm (arme de poing)
    WPN_CLAWS,          // griffes des poussins zombies (bots) : ni chargeur ni portee
    WPN_COUNT
} WeaponId;

// Mode de tir
typedef enum {
    FIRE_AUTO = 0,      // tant que le bouton est maintenu
    FIRE_SEMI,          // un coup par pression
    FIRE_BOLT,          // un coup par pression, puis rearmement (verrou)
    FIRE_PUMP,          // un coup par pression, puis pompe
} FireMode;

// Actions de PKT_CL_WEAPON
typedef enum {
    WA_LOADOUT = 0,     // a = arme principale, b = arme de poing
    WA_SWITCH,          // a = emplacement (0 = principale, 1 = poing)
    WA_RELOAD,
} WeaponAction;

#define WEAPON_SLOTS    2
#define SLOT_PRIMARY    0
#define SLOT_SECONDARY  1

#pragma pack(push, 1)

typedef struct {
    uint8_t type;           // PacketType
} PacketHeader;

typedef struct {
    uint8_t type;           // PKT_CL_CONNECT
    uint8_t version;        // PROTO_VERSION
    uint8_t is_bot;         // BotKind
    char    name[NAME_MAX];
} PacketConnect;

typedef struct {
    uint8_t  type;          // PKT_SV_ACCEPT
    uint8_t  player_id;
    uint8_t  team;          // Team
} PacketAccept;

typedef struct {
    uint8_t type;           // PKT_SV_REJECT
    char    reason[32];
} PacketReject;

// Une entrée du joueur : ce qu'il a fait pendant UNE image. Le client en
// produit une par image et les simule aussitôt (prédiction) ; le serveur
// les rejoue dans l'ordre avec le MÊME code (PlayerStep, game.c) et fait
// autorité. Le déplacement est quantifié sur un octet : les deux côtés
// simulent la valeur déquantifiée, pour que la prédiction soit exacte.
typedef struct {
    uint16_t seq;           // numéro d'entrée, +1 par image (boucle à 65536)
    float    dt;            // durée simulée par cette entrée (s), <= INPUT_MAX_DT
    int8_t   move_x;        // -127..127 : vers la droite
    int8_t   move_y;        // -127..127 : vers l'avant
    uint8_t  flags;         // INPUT_JUMP | INPUT_SPRINT | INPUT_ADS
    uint8_t  pad;
    float    yaw, pitch;    // la vue, en radians (le serveur la relaie telle quelle)
} InputEntry;

// Paquet d'entrées : les INPUT_BATCH dernières, de la plus ancienne à la
// plus récente. Le serveur ignore celles qu'il a déjà vues (seq <= dernier
// traité) : la redondance protège des pertes UDP sans rien dupliquer.
// spawn_count : écho du compteur de respawn reçu dans les snapshots. Le
// serveur ignore les entrées d'un compteur périmé : sans cela, une entrée
// partie juste AVANT que le client apprenne son respawn ferait bouger le
// joueur depuis l'endroit de sa mort.
typedef struct {
    uint8_t    type;        // PKT_CL_INPUT
    uint8_t    player_id;
    uint8_t    spawn_count;
    uint8_t    count;       // entrées valides dans entries[] (1..INPUT_BATCH)
    InputEntry entries[INPUT_BATCH];
} PacketClientInput;

typedef struct {
    uint8_t  type;          // PKT_CL_SHOOT
    uint8_t  player_id;
    uint8_t  slot;          // emplacement de l'arme avec laquelle le client croit tirer
    uint8_t  pad;
    uint16_t shot_seq;      // numero du tir, +1 par tir : le serveur en accuse reception (last_shot_seq)
    float    ox, oy, oz;    // origine du rayon (caméra)
    float    dx, dy, dz;    // direction normalisée
    float    view_time;     // instant serveur (s) que le tireur VOYAIT (voir INTERP_DELAY)
} PacketShoot;

typedef struct {
    uint8_t type;           // PKT_CL_DISCONNECT
    uint8_t player_id;
} PacketDisconnect;

typedef struct {
    uint8_t type;           // PKT_CL_WEAPON
    uint8_t player_id;
    uint8_t action;         // WeaponAction
    uint8_t a, b;           // selon l'action
} PacketWeapon;

// Un joueur dans un snapshot
typedef struct {
    uint8_t  id;
    uint8_t  active;        // 1 si le slot est occupé
    uint8_t  team;
    uint8_t  alive;
    uint8_t  is_bot;        // BotKind : 1 = zombie (poussin), 0 ou 2 = soldat
    uint8_t  spawn_count;   // +1 à chaque respawn (voir PacketClientState)
    int16_t  health;
    uint16_t kills;
    uint16_t deaths;
    float    x, y, z;
    float    yaw, pitch;
    // Pour la réconciliation du joueur LOCAL : l'état complet de sa
    // simulation après la dernière entrée traitée (last_seq). Il repart de
    // là et rejoue ses entrées plus récentes.
    float    vx, vz;        // vitesse horizontale (accélération progressive)
    float    vy;            // vitesse verticale
    uint8_t  on_ground;
    uint16_t last_seq;      // dernière entrée traitée par le serveur
    // Armes : ce que chacun tient (pour le dessiner), et pour le joueur
    // LOCAL l'etat de ses chargeurs.
    uint8_t  weapon[WEAPON_SLOTS];  // WeaponId par emplacement
    uint8_t  slot;                  // emplacement actif
    uint8_t  mag[WEAPON_SLOTS];     // cartouches dans chaque chargeur
    uint16_t reserve[WEAPON_SLOTS]; // cartouches en reserve
    float    reload_t;              // secondes de rechargement restantes (0 = pas en cours)
    uint16_t last_shot_seq;         // dernier tir recu (accuse ou non) : voir PacketShoot
    char     name[NAME_MAX];
} SnapshotPlayer;

typedef struct {
    uint8_t        type;    // PKT_SV_SNAPSHOT
    uint32_t       tick;
    uint16_t       score[TEAM_COUNT];   // MANCHES gagnées par équipe
    uint8_t        match_over;      // 1 si le match est fini
    uint8_t        winner;          // équipe gagnante si match_over
    uint8_t        round;           // numéro de la manche en cours (1..)
    uint8_t        round_phase;     // RoundPhase
    uint8_t        round_winner;    // en ROUND_END : équipe gagnante, TEAM_COUNT = nulle
    float          round_time;      // secondes restantes (PLAY) ou avant la suivante (END)
    // La bombe
    uint8_t        bomb_state;      // BombState
    uint8_t        bomb_carrier;    // id du porteur (CARRIED)
    uint8_t        bomb_site;       // 0 = A, 1 = B (PLANTED)
    uint8_t        bomb_actor;      // joueur en train de poser ou de desamorcer, 255 = personne
    float          bomb_x, bomb_y, bomb_z;   // position (DROPPED, PLANTED)
    float          bomb_timer;      // secondes avant l'explosion (PLANTED)
    float          bomb_progress;   // 0..1 : avancement de la pose ou du desamorcage
    SnapshotPlayer players[MAX_PLAYERS];
} PacketSnapshot;

typedef struct {
    uint8_t  type;          // PKT_SV_EVENT
    uint8_t  event;         // EventType
    uint8_t  a, b;          // ids de joueurs concernés (ou équipe)
    int16_t  value;         // donnée annexe (dégâts...)
} PacketEvent;

#pragma pack(pop)

#endif // PROTOCOL_H
