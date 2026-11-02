/*
 * bot/main.c — Bot "zombie" headless (sans fenêtre), pour le développement.
 *
 * Usage : ./bot [ip_serveur] [nom]
 *
 * Le bot se connecte au serveur EXACTEMENT comme un vrai client (même
 * protocole UDP), mais sans raylib ni affichage : parfait pour tester le
 * jeu tout seul. Comportement de zombie :
 *   - il repère le joueur ennemi vivant le plus proche,
 *   - il marche vers lui en ligne droite (plus lentement qu'un joueur),
 *     en glissant le long des obstacles grâce aux collisions partagées,
 *   - s'il reste coincé contre un mur, il saute et fait un pas de côté,
 *   - arrivé au corps-à-corps, il "griffe" : techniquement c'est un tir
 *     (PKT_CL_SHOOT) à bout portant vers la poitrine de la cible, mais
 *     limité par un cooldown d'attaque bien plus lent qu'une arme.
 *
 * Comme pour un client normal, le serveur reste autoritaire : il valide
 * l'origine de chaque attaque, applique les dégâts et gère les respawns.
 */

// platform.h d'abord : sockets, horloge monotone et sleep, quel que soit
// le systeme (voir src/common/platform.h).
#include "../common/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/protocol.h"
#include "../common/game.h"

//----------------------------------------------------------------------------------
// Réglages du zombie
//----------------------------------------------------------------------------------
#define ZOMBIE_SPEED        3.6f   // m/s : environ moitié d'un joueur (7 m/s)
#define ZOMBIE_ATTACK_RANGE 2.0f   // distance de griffe (m)
#define ZOMBIE_ATTACK_CD    0.9f   // secondes entre deux griffes
#define ZOMBIE_TICK_HZ      60     // fréquence de la simulation locale

//----------------------------------------------------------------------------------
// État du bot
//----------------------------------------------------------------------------------
static socket_t           sock_fd = SOCKET_NONE;
static struct sockaddr_in server_addr;
static int                my_id = -1;
static Team               my_team = TEAM_RED;

static PacketSnapshot snap;        // dernier état du monde reçu du serveur
static bool           has_snap = false;

static V3    pos;                  // position des pieds (simulée localement)
static float vel_y = 0.0f;         // vitesse verticale (gravité / sauts)
static float yaw = 0.0f;           // orientation (pour que les clients le voient tourné)
static bool  alive = true;
static uint8_t spawn_count = 0;    // compteur de respawn (voir protocol.h)
static float attack_cd = 0.0f;     // temps avant la prochaine griffe

// Détection de blocage : si on veut avancer mais qu'on ne bouge presque
// pas, on est coincé contre un obstacle -> saut + pas de côté
static V3    last_pos;
static float stuck_time = 0.0f;    // depuis combien de temps on est coincé
static float dodge_time = 0.0f;    // temps restant de pas de côté
static int   dodge_side = 1;       // -1 = gauche, +1 = droite

// Horloge monotone de la boucle du bot (voir platform.h)
static double Now(void) { return PlatformNow(); }

//----------------------------------------------------------------------------------
// Réseau (même logique que le client graphique)
//----------------------------------------------------------------------------------
// Poignée de main : on renvoie PKT_CL_CONNECT jusqu'à l'acceptation
static bool NetConnect(const char *ip, const char *name)
{
    if (!PlatformNetInit()) { printf("pile reseau indisponible\n"); return false; }
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (SocketFailed(sock_fd)) { perror("socket"); return false; }
    SocketSetNonBlocking(sock_fd);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(DEFAULT_PORT);
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) != 1) {
        printf("Adresse IP invalide : %s\n", ip);
        return false;
    }

    PacketConnect pkt = {0};
    pkt.type = PKT_CL_CONNECT;
    pkt.version = PROTO_VERSION;
    pkt.is_bot = 1;                // les clients nous dessineront en poussin zombie
    snprintf(pkt.name, NAME_MAX, "%s", name);

    for (int attempt = 0; attempt < 10; attempt++) {
        sendto(sock_fd, &pkt, sizeof(pkt), 0,
               (struct sockaddr *)&server_addr, sizeof(server_addr));

        double deadline = Now() + 0.5;
        while (Now() < deadline) {
            uint8_t buf[2048];
            sock_ssize_t n = recvfrom(sock_fd, buf, sizeof(buf), 0, NULL, NULL);
            if (n >= (sock_ssize_t)sizeof(PacketAccept) && buf[0] == PKT_SV_ACCEPT) {
                PacketAccept *acc = (PacketAccept *)buf;
                my_id = acc->player_id;
                my_team = (Team)acc->team;
                printf("[zombie] connecté : id %d, équipe %s\n", my_id,
                       my_team == TEAM_RED ? "ROUGE" : "BLEUE");
                return true;
            }
            if (n >= (sock_ssize_t)sizeof(PacketHeader) && buf[0] == PKT_SV_REJECT) {
                printf("[zombie] connexion refusée\n");
                return false;
            }
            PlatformSleepMs(10);
        }
        printf("[zombie] connexion à %s... (%d/10)\n", ip, attempt + 1);
    }
    printf("[zombie] serveur injoignable\n");
    return false;
}

static void NetSendState(void)
{
    PacketClientState pkt = {
        PKT_CL_STATE, (uint8_t)my_id, spawn_count,
        pos.x, pos.y, pos.z,
        yaw, 0.0f,                 // un zombie regarde droit devant
    };
    sendto(sock_fd, &pkt, sizeof(pkt), 0,
           (struct sockaddr *)&server_addr, sizeof(server_addr));
}

static void NetSendAttack(V3 origin, V3 dir)
{
    PacketShoot pkt = {
        PKT_CL_SHOOT, (uint8_t)my_id,
        origin.x, origin.y, origin.z,
        dir.x, dir.y, dir.z,
    };
    sendto(sock_fd, &pkt, sizeof(pkt), 0,
           (struct sockaddr *)&server_addr, sizeof(server_addr));
}

// Lit tous les paquets en attente. On ne garde que les snapshots : les
// événements (kills...) ne servent à rien à un zombie.
static void NetReceive(void)
{
    for (;;) {
        uint8_t buf[4096];
        sock_ssize_t n = recvfrom(sock_fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n <= 0) break;

        if (buf[0] == PKT_SV_SNAPSHOT && n >= (sock_ssize_t)sizeof(PacketSnapshot)) {
            memcpy(&snap, buf, sizeof(snap));
            has_snap = true;

            // Le serveur décide de notre vie et de nos respawns : quand
            // son compteur de respawn change, on adopte la position qu'il
            // a choisie et on écho le compteur dans nos paquets d'état.
            SnapshotPlayer *me = &snap.players[my_id];
            if (me->active) {
                if (me->spawn_count != spawn_count) {
                    spawn_count = me->spawn_count;
                    pos = (V3){me->x, me->y, me->z};
                    vel_y = 0.0f;
                    printf("[zombie] respawn en (%.0f, %.0f)\n", pos.x, pos.z);
                }
                alive = me->alive != 0;
            }
        }
    }
}

//----------------------------------------------------------------------------------
// Cerveau du zombie
//----------------------------------------------------------------------------------
// Cherche l'ennemi vivant le plus proche dans le dernier snapshot.
// Distance mesurée à plat (X/Z). Renvoie NULL si personne à chasser.
static SnapshotPlayer *NearestEnemy(float *dist_out)
{
    SnapshotPlayer *best = NULL;
    float best_d = 1e9f;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        SnapshotPlayer *p = &snap.players[i];
        if (!p->active || !p->alive || i == my_id) continue;
        if (p->team == my_team) continue;   // on ne mange pas ses coéquipiers
        float dx = p->x - pos.x, dz = p->z - pos.z;
        float d = sqrtf(dx * dx + dz * dz);
        if (d < best_d) { best_d = d; best = p; }
    }
    if (dist_out) *dist_out = best_d;
    return best;
}

// Un pas de simulation du zombie (appelé ZOMBIE_TICK_HZ fois par seconde)
static void ZombieThink(float dt)
{
    if (attack_cd > 0.0f) attack_cd -= dt;
    if (!alive || !has_snap || snap.match_over) return;

    float dist;
    SnapshotPlayer *target = NearestEnemy(&dist);

    // Personne à chasser : le zombie reste planté là (la gravité
    // continue de s'appliquer, au cas où il serait en l'air)
    if (!target) {
        bool grounded;
        MovePlayer(&pos, (V3){0}, &vel_y, dt, &grounded);
        return;
    }

    // Direction vers la cible, à plat, normalisée
    V3 to_target = {target->x - pos.x, 0, target->z - pos.z};
    float len = sqrtf(to_target.x * to_target.x + to_target.z * to_target.z);
    if (len > 0.001f) { to_target.x /= len; to_target.z /= len; }
    yaw = atan2f(to_target.x, to_target.z);   // il regarde sa proie

    // --- Déplacement : droit sur la cible, sauf pendant un pas de côté
    // (anti-blocage). On s'arrête un peu avant le contact pour ne pas
    // pousser la cible. ---
    V3 vel = {0};
    if (dist > ZOMBIE_ATTACK_RANGE * 0.7f) {
        vel.x = to_target.x * ZOMBIE_SPEED;
        vel.z = to_target.z * ZOMBIE_SPEED;
        if (dodge_time > 0.0f) {
            // Pas de côté : on remplace le mouvement par un déplacement
            // perpendiculaire (vecteur "droite" = (z, -x) du vecteur avant)
            dodge_time -= dt;
            vel.x = to_target.z * ZOMBIE_SPEED * (float)dodge_side;
            vel.z = -to_target.x * ZOMBIE_SPEED * (float)dodge_side;
        }
    }

    bool grounded;
    MovePlayer(&pos, vel, &vel_y, dt, &grounded);

    // --- Anti-blocage : on voulait avancer mais on n'a presque pas bougé
    // pendant 0,4 s -> on saute (obstacle bas) et on fait un pas de côté
    // d'une demi-seconde dans un sens aléatoire (contournement grossier) ---
    bool wants_to_move = (vel.x != 0.0f || vel.z != 0.0f);
    float moved = fabsf(pos.x - last_pos.x) + fabsf(pos.z - last_pos.z);
    if (wants_to_move && moved < ZOMBIE_SPEED * dt * 0.3f) {
        stuck_time += dt;
        if (stuck_time > 0.4f) {
            stuck_time = 0.0f;
            if (grounded) vel_y = PLAYER_JUMP_SPEED;   // tente de sauter par-dessus
            dodge_time = 0.5f;
            dodge_side = (rand() % 2) ? 1 : -1;
        }
    } else {
        stuck_time = 0.0f;
    }
    last_pos = pos;

    // --- Griffe : à portée et cooldown écoulé. L'attaque part de nos
    // yeux vers la poitrine de la cible (comme un tir à bout portant,
    // c'est le serveur qui vérifie et applique les dégâts). ---
    if (dist < ZOMBIE_ATTACK_RANGE && attack_cd <= 0.0f) {
        attack_cd = ZOMBIE_ATTACK_CD;
        V3 eye = {pos.x, pos.y + PLAYER_EYE_HEIGHT, pos.z};
        V3 chest = {target->x, target->y + 1.0f, target->z};
        V3 dir = {chest.x - eye.x, chest.y - eye.y, chest.z - eye.z};
        float dlen = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (dlen > 0.001f) {
            dir.x /= dlen; dir.y /= dlen; dir.z /= dlen;
            NetSendAttack(eye, dir);
            printf("[zombie] griffe %s (dist %.1f m)\n", target->name, dist);
        }
    }
}

//----------------------------------------------------------------------------------
// Boucle principale : simulation à 60 Hz, envoi d'état à 30 Hz
//----------------------------------------------------------------------------------
int main(int argc, char **argv)
{
    const char *ip = (argc > 1) ? argv[1] : "127.0.0.1";
    const char *name = (argc > 2) ? argv[2] : "Zombie";
    srand((unsigned)(time(NULL) ^ getpid()));
    setvbuf(stdout, NULL, _IOLBF, 0);   // logs visibles même redirigés

    if (!NetConnect(ip, name)) return 1;

    // Position de départ : premier spawn de notre équipe (le premier
    // snapshot du serveur corrigera)
    pos = TEAM_SPAWNS[my_team][0];
    last_pos = pos;
    memset(&snap, 0, sizeof(snap));

    const double tick_dt = 1.0 / ZOMBIE_TICK_HZ;
    const double send_dt = 1.0 / CLIENT_SEND_HZ;
    double last_tick = Now();
    double last_send = Now();

    for (;;) {
        NetReceive();

        double now = Now();
        if (now - last_tick >= tick_dt) {
            float dt = (float)(now - last_tick);
            if (dt > 0.05f) dt = 0.05f;   // même garde-fou que le client
            last_tick = now;
            ZombieThink(dt);
        }

        // L'envoi d'état sert aussi de battement de cœur : sans lui le
        // serveur nous déconnecterait au timeout de 5 s
        if (now - last_send >= send_dt) {
            last_send = now;
            NetSendState();
        }

        PlatformSleepMs(2);   // ~2 ms : ne sature pas un cœur de CPU
    }
    return 0;
}
