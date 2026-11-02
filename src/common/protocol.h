// protocol.h — Protocole réseau partagé client/serveur (UDP)
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define PROTO_VERSION   1
#define DEFAULT_PORT    7777

#define MAX_PLAYERS     16
#define NAME_MAX        16

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
    PKT_CL_STATE,           // position/orientation du joueur
    PKT_CL_SHOOT,           // tir (origine + direction)
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
} EventType;

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

// État envoyé par le client (mouvement prédit côté client,
// le serveur reste autoritaire sur la vie/les scores).
// spawn_count : écho du compteur de respawn reçu dans les snapshots.
// Le serveur ignore les positions dont le compteur ne correspond pas :
// sans cela, un paquet de position parti juste AVANT que le client
// apprenne son respawn le ramènerait à l'endroit de sa mort.
typedef struct {
    uint8_t  type;          // PKT_CL_STATE
    uint8_t  player_id;
    uint8_t  spawn_count;
    float    x, y, z;
    float    yaw, pitch;    // en radians
} PacketClientState;

typedef struct {
    uint8_t  type;          // PKT_CL_SHOOT
    uint8_t  player_id;
    float    ox, oy, oz;    // origine du rayon (caméra)
    float    dx, dy, dz;    // direction normalisée
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
    char     name[NAME_MAX];
} SnapshotPlayer;

typedef struct {
    uint8_t        type;    // PKT_SV_SNAPSHOT
    uint32_t       tick;
    uint16_t       score[TEAM_COUNT];
    uint8_t        match_over;      // 1 si le match est fini
    uint8_t        winner;          // équipe gagnante si match_over
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
