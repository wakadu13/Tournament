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
#define PROTO_VERSION   3
#define DEFAULT_PORT    7777

#define MAX_PLAYERS     16
#define NAME_MAX        16

// Entrees du joueur
#define INPUT_BATCH     8       // entrees par paquet : les dernieres, repetees d'un paquet a l'autre (pertes)
#define INPUT_MAX_DT    0.05f   // duree maximale simulee par une entree (s) : au-dela, triche ou gel
#define INPUT_JUMP      0x01
#define INPUT_SPRINT    0x02
#define INPUT_ADS       0x04

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
    EV_KILL = 1,        // a = tueur, b = victime
    EV_HIT,             // a = tireur, b = touché, value = dégâts
    EV_RESPAWN,         // a = joueur
    EV_PLAYER_JOIN,     // a = joueur
    EV_PLAYER_LEAVE,    // a = joueur
    EV_MATCH_OVER,      // a = équipe gagnante
    EV_ROUND_START,     // a = numéro de manche
    EV_ROUND_END,       // a = équipe gagnante (TEAM_COUNT = manche nulle), b = numéro
} EventType;

// Phase de la manche en cours
typedef enum {
    ROUND_WAIT = 0,     // il manque une équipe : on attend
    ROUND_PLAY,         // manche en cours
    ROUND_END,          // manche finie, pause avant la suivante
} RoundPhase;

#pragma pack(push, 1)

typedef struct {
    uint8_t type;           // PacketType
} PacketHeader;

typedef struct {
    uint8_t type;           // PKT_CL_CONNECT
    uint8_t version;        // PROTO_VERSION
    uint8_t is_bot;         // 1 = bot zombie, 0 = joueur humain
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
    float    ox, oy, oz;    // origine du rayon (caméra)
    float    dx, dy, dz;    // direction normalisée
    float    view_time;     // instant serveur (s) que le tireur VOYAIT (voir INTERP_DELAY)
} PacketShoot;

typedef struct {
    uint8_t type;           // PKT_CL_DISCONNECT
    uint8_t player_id;
} PacketDisconnect;

// Un joueur dans un snapshot
typedef struct {
    uint8_t  id;
    uint8_t  active;        // 1 si le slot est occupé
    uint8_t  team;
    uint8_t  alive;
    uint8_t  is_bot;        // 1 = zombie (poussin), 0 = humain (soldat)
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
