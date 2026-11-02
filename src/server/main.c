// server/main.c — Serveur dédié TDM (UDP, autoritaire sur vie/scores)
//
// Usage : ./server [port]
//
// Le client simule son propre déplacement (prédiction) et envoie sa
// position ; le serveur reste autoritaire sur les tirs, la vie, les
// kills, les scores et les respawns.

// platform.h d'abord : sockets, horloge monotone et sleep, quel que soit
// le systeme (voir src/common/platform.h).
#include "../common/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/protocol.h"
#include "../common/game.h"

typedef struct {
    bool               active;
    struct sockaddr_in addr;
    char               name[NAME_MAX];
    Team               team;
    bool               is_bot;      // relayé aux clients pour le rendu

    V3    pos;
    float yaw, pitch;

    bool    alive;
    uint8_t spawn_count;    // +1 à chaque respawn (voir protocol.h)
    int   health;
    int   kills, deaths;
    float respawn_timer;
    float shoot_cooldown;
    double last_heard;      // pour le timeout
} Player;

static Player   players[MAX_PLAYERS];
static int      team_score[TEAM_COUNT];
static bool     match_over = false;
static int      winner = 0;
static uint32_t tick = 0;
static socket_t sock_fd = SOCKET_NONE;

// Horloge monotone de la boucle de simulation (voir platform.h)
static double Now(void) { return PlatformNow(); }

static bool SameAddr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void SendTo(const struct sockaddr_in *addr, const void *data, size_t len)
{
    sendto(sock_fd, data, len, 0, (const struct sockaddr *)addr, sizeof(*addr));
}

static void Broadcast(const void *data, size_t len)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].active) SendTo(&players[i].addr, data, len);
    }
}

static void BroadcastEvent(EventType ev, int a, int b, int value)
{
    PacketEvent p = {PKT_SV_EVENT, (uint8_t)ev, (uint8_t)a, (uint8_t)b, (int16_t)value};
    Broadcast(&p, sizeof(p));
}

static void SpawnPlayer(Player *p)
{
    int idx = rand() % SPAWNS_PER_TEAM;
    p->pos = TEAM_SPAWNS[p->team][idx];
    p->yaw = (p->team == TEAM_RED) ? 0.0f : (float)M_PI; // face au centre
    p->pitch = 0.0f;
    p->health = PLAYER_MAX_HEALTH;
    p->alive = true;
    p->spawn_count++;       // invalide les paquets de position d'avant la mort
    p->respawn_timer = 0.0f;
    p->shoot_cooldown = 0.0f;
}

static int CountTeam(Team t)
{
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].active && players[i].team == t) n++;
    }
    return n;
}

static void HandleConnect(const PacketConnect *pkt, const struct sockaddr_in *from)
{
    // Déjà connecté depuis cette adresse ? On renvoie l'accept (paquet perdu).
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].active && SameAddr(&players[i].addr, from)) {
            PacketAccept acc = {PKT_SV_ACCEPT, (uint8_t)i, (uint8_t)players[i].team};
            SendTo(from, &acc, sizeof(acc));
            return;
        }
    }

    if (pkt->version != PROTO_VERSION) {
        PacketReject rej = {PKT_SV_REJECT, "version incompatible"};
        SendTo(from, &rej, sizeof(rej));
        return;
    }

    int slot = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!players[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        PacketReject rej = {PKT_SV_REJECT, "serveur plein"};
        SendTo(from, &rej, sizeof(rej));
        return;
    }

    // Auto-équilibrage : on rejoint l'équipe la moins nombreuse
    // (calculé avant d'activer le slot pour ne pas se compter soi-même)
    Team team = (CountTeam(TEAM_RED) <= CountTeam(TEAM_BLUE)) ? TEAM_RED : TEAM_BLUE;

    Player *p = &players[slot];
    memset(p, 0, sizeof(*p));
    p->active = true;
    p->addr = *from;
    memcpy(p->name, pkt->name, NAME_MAX);
    p->name[NAME_MAX - 1] = '\0';
    p->team = team;
    p->is_bot = pkt->is_bot != 0;
    p->last_heard = Now();
    SpawnPlayer(p);

    PacketAccept acc = {PKT_SV_ACCEPT, (uint8_t)slot, (uint8_t)p->team};
    SendTo(from, &acc, sizeof(acc));
    BroadcastEvent(EV_PLAYER_JOIN, slot, 0, 0);
    printf("[+] %s rejoint (slot %d, équipe %s)\n",
           p->name, slot, p->team == TEAM_RED ? "ROUGE" : "BLEUE");
}

static void RemovePlayer(int id, const char *why)
{
    if (!players[id].active) return;
    printf("[-] %s parti (%s)\n", players[id].name, why);
    players[id].active = false;
    BroadcastEvent(EV_PLAYER_LEAVE, id, 0, 0);
}

// Vérifie que le paquet vient bien de l'adresse associée à cet id
static Player *ValidSender(int id, const struct sockaddr_in *from)
{
    if (id < 0 || id >= MAX_PLAYERS) return NULL;
    Player *p = &players[id];
    if (!p->active || !SameAddr(&p->addr, from)) return NULL;
    p->last_heard = Now();
    return p;
}

static void HandleShoot(const PacketShoot *pkt, const struct sockaddr_in *from)
{
    Player *shooter = ValidSender(pkt->player_id, from);
    if (!shooter || !shooter->alive || match_over) return;
    if (shooter->shoot_cooldown > 0.0f) return;
    shooter->shoot_cooldown = WEAPON_COOLDOWN;

    V3 o = {pkt->ox, pkt->oy, pkt->oz};
    V3 d = {pkt->dx, pkt->dy, pkt->dz};
    float dlen = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (dlen < 0.5f || dlen > 1.5f) return; // direction invalide
    d.x /= dlen; d.y /= dlen; d.z /= dlen;

    // Anti-triche basique : l'origine doit être proche de la tête du joueur
    V3 eye = {shooter->pos.x, shooter->pos.y + PLAYER_EYE_HEIGHT, shooter->pos.z};
    float ex = o.x - eye.x, ey = o.y - eye.y, ez = o.z - eye.z;
    if (ex * ex + ey * ey + ez * ez > 4.0f) return;

    float wall_t = RaycastMap(o, d);

    // Joueur le plus proche touché avant un mur
    int   hit_id = -1;
    float hit_t = wall_t;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *t = &players[i];
        if (!t->active || !t->alive || i == pkt->player_id) continue;
        if (t->team == shooter->team) continue; // pas de friendly fire
        float ht;
        if (RayVsBox(o, d, PlayerBox(t->pos), &ht) && ht > 0.0f && ht < hit_t) {
            hit_t = ht;
            hit_id = i;
        }
    }

    if (hit_id < 0) return;

    Player *victim = &players[hit_id];
    victim->health -= WEAPON_DAMAGE;
    BroadcastEvent(EV_HIT, pkt->player_id, hit_id, WEAPON_DAMAGE);

    if (victim->health <= 0) {
        victim->alive = false;
        victim->deaths++;
        victim->respawn_timer = RESPAWN_DELAY;
        shooter->kills++;
        team_score[shooter->team]++;
        BroadcastEvent(EV_KILL, pkt->player_id, hit_id, 0);
        printf("[*] %s a éliminé %s (%d - %d)\n", shooter->name, victim->name,
               team_score[TEAM_RED], team_score[TEAM_BLUE]);

        if (team_score[shooter->team] >= SCORE_LIMIT) {
            match_over = true;
            winner = shooter->team;
            BroadcastEvent(EV_MATCH_OVER, winner, 0, 0);
            printf("[*] MATCH TERMINÉ — victoire équipe %s\n",
                   winner == TEAM_RED ? "ROUGE" : "BLEUE");
        }
    }
}

static void HandlePacket(const uint8_t *buf, sock_ssize_t len, const struct sockaddr_in *from)
{
    if (len < (sock_ssize_t)sizeof(PacketHeader)) return;

    switch (buf[0]) {
    case PKT_CL_CONNECT:
        if (len >= (sock_ssize_t)sizeof(PacketConnect))
            HandleConnect((const PacketConnect *)buf, from);
        break;

    case PKT_CL_STATE:
        if (len >= (sock_ssize_t)sizeof(PacketClientState)) {
            const PacketClientState *pkt = (const PacketClientState *)buf;
            Player *p = ValidSender(pkt->player_id, from);
            // spawn_count : on rejette les positions datant d'avant le
            // dernier respawn (sinon elles ramèneraient le joueur à
            // l'endroit de sa mort)
            if (p && p->alive && pkt->spawn_count == p->spawn_count) {
                // TODO anti-triche : valider la vitesse de déplacement
                p->pos = (V3){pkt->x, pkt->y, pkt->z};
                p->yaw = pkt->yaw;
                p->pitch = pkt->pitch;
            }
        }
        break;

    case PKT_CL_SHOOT:
        if (len >= (sock_ssize_t)sizeof(PacketShoot))
            HandleShoot((const PacketShoot *)buf, from);
        break;

    case PKT_CL_DISCONNECT:
        if (len >= (sock_ssize_t)sizeof(PacketDisconnect)) {
            const PacketDisconnect *pkt = (const PacketDisconnect *)buf;
            if (ValidSender(pkt->player_id, from))
                RemovePlayer(pkt->player_id, "déconnexion");
        }
        break;
    }
}

static void SendSnapshot(void)
{
    PacketSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.type = PKT_SV_SNAPSHOT;
    snap.tick = tick;
    snap.score[TEAM_RED] = (uint16_t)team_score[TEAM_RED];
    snap.score[TEAM_BLUE] = (uint16_t)team_score[TEAM_BLUE];
    snap.match_over = match_over ? 1 : 0;
    snap.winner = (uint8_t)winner;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &players[i];
        SnapshotPlayer *s = &snap.players[i];
        s->id = (uint8_t)i;
        s->active = p->active ? 1 : 0;
        if (!p->active) continue;
        s->team = (uint8_t)p->team;
        s->alive = p->alive ? 1 : 0;
        s->is_bot = p->is_bot ? 1 : 0;
        s->spawn_count = p->spawn_count;
        s->health = (int16_t)p->health;
        s->kills = (uint16_t)p->kills;
        s->deaths = (uint16_t)p->deaths;
        s->x = p->pos.x; s->y = p->pos.y; s->z = p->pos.z;
        s->yaw = p->yaw; s->pitch = p->pitch;
        memcpy(s->name, p->name, NAME_MAX);
    }
    Broadcast(&snap, sizeof(snap));
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    srand((unsigned)time(NULL));
    setvbuf(stdout, NULL, _IOLBF, 0); // logs visibles même redirigés

    if (!PlatformNetInit()) { printf("pile reseau indisponible\n"); return 1; }
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (SocketFailed(sock_fd)) { perror("socket"); return 1; }
    SocketSetNonBlocking(sock_fd);

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons((uint16_t)port);
    if (bind(sock_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind");
        return 1;
    }

    printf("Serveur TDM démarré sur le port %d (tick %d Hz)\n", port, SERVER_TICK_HZ);

    const double tick_dt = 1.0 / SERVER_TICK_HZ;
    const double snap_dt = 1.0 / SNAPSHOT_HZ;
    double last_tick = Now();
    double last_snap = Now();

    for (;;) {
        // Réception de tous les paquets en attente
        for (;;) {
            uint8_t buf[2048];
            struct sockaddr_in from;
            sock_len_t from_len = sizeof(from);
            sock_ssize_t n = recvfrom(sock_fd, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&from, &from_len);
            if (n <= 0) break;
            HandlePacket(buf, n, &from);
        }

        double now = Now();
        if (now - last_tick >= tick_dt) {
            float dt = (float)(now - last_tick);
            last_tick = now;
            tick++;

            for (int i = 0; i < MAX_PLAYERS; i++) {
                Player *p = &players[i];
                if (!p->active) continue;

                if (now - p->last_heard > NET_TIMEOUT_SEC) {
                    RemovePlayer(i, "timeout");
                    continue;
                }
                if (p->shoot_cooldown > 0.0f) p->shoot_cooldown -= dt;
                if (!p->alive && !match_over) {
                    p->respawn_timer -= dt;
                    if (p->respawn_timer <= 0.0f) {
                        SpawnPlayer(p);
                        BroadcastEvent(EV_RESPAWN, i, 0, 0);
                    }
                }
            }
        }

        if (now - last_snap >= snap_dt) {
            last_snap = now;
            SendSnapshot();
        }

        PlatformSleepMs(1);   // ~1 ms, évite de saturer un cœur
    }
    return 0;
}
