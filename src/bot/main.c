/*
 * bot/main.c — Bot headless (sans fenêtre) : poussin zombie ou soldat armé.
 *
 * Usage : ./bot [ip_serveur] [nom] [zombie|soldat] [adresse 0..3]
 *
 * Le SOLDAT (is_bot = BOT_SOLDIER) joue comme un joueur : il choisit une
 * arme au hasard, suit les points de passage vers l'ennemi le plus proche,
 * et dès qu'il le voit il le vise (avec une erreur qui dépend de son
 * adresse et de la distance), tire selon le mode de son arme (rafales
 * pour les automatiques), recharge à vide, passe au pistolet sans
 * réserve, garde la distance qui convient à son arme en se décalant. Les
 * T portent et posent la bombe sur un site, les CT la désamorcent.
 *
 * Le bot se connecte au serveur EXACTEMENT comme un vrai client (même
 * protocole UDP), mais sans raylib ni affichage : parfait pour tester le
 * jeu tout seul. Comportement de zombie :
 *   - il repère le joueur ennemi vivant le plus proche,
 *   - s'il le voit, il marche droit sur lui (plus lentement qu'un joueur) ;
 *     sinon il suit les POINTS DE PASSAGE de la carte (NAV_NODES, game.c) :
 *     du nœud le plus proche de lui au nœud le plus proche de sa proie, par
 *     le chemin le plus court en nombre d'étapes,
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

// Réglages du soldat
#define SOLDIER_FIGHT_K     0.6f   // vitesse en combat (fraction de la vitesse max)
#define SOLDIER_SEE_RANGE   60.0f  // au-delà, il ne « voit » pas (il suit les points de passage)

//----------------------------------------------------------------------------------
// État du bot
//----------------------------------------------------------------------------------
static socket_t           sock_fd = SOCKET_NONE;
static struct sockaddr_in server_addr;
static int                my_id = -1;
static Team               my_team = TEAM_RED;

static PacketSnapshot snap;        // dernier état du monde reçu du serveur
static bool           has_snap = false;

static V3    pos;                  // position des pieds, LUE dans les snapshots (le serveur simule)
static float yaw = 0.0f;           // orientation (pour que les clients le voient tourné)
static uint32_t pos_tick = 0;      // tick du snapshot d'où vient pos

// Entrées : le bot n'envoie plus de position mais ce qu'il VEUT faire, et
// le serveur le déplace avec le même code que pour un joueur. Une entrée
// par tick, les INPUT_BATCH dernières dans chaque paquet (redondance).
static InputEntry pending[INPUT_BATCH];
static int        pending_n = 0;   // entrées dans pending[], la plus récente en dernier
static uint16_t   my_seq = 0;
static float      jump_hold = 0.0f;   // secondes pendant lesquelles on demande à sauter
static bool  alive = true;
static uint8_t spawn_count = 0;    // compteur de respawn (voir protocol.h)
static float attack_cd = 0.0f;     // temps avant la prochaine griffe
static int   kind = BOT_ZOMBIE;    // BotKind
static int   skill = 2;            // 0 = maladroit .. 3 = redoutable (soldat)
static bool  debug = false;        // EMA_BOT_DEBUG=1 : journal du cerveau du soldat
static float pitch = 0.0f;         // le soldat lève ou baisse son arme
static bool  use_held = false;     // action maintenue (bombe)
static uint16_t shot_seq = 0;

// Soldat : tir et munitions
static int   burst_left = 0;       // coups restants dans la rafale en cours
static float fire_pause = 0.0f;    // s avant de reprendre le tir
static float reload_wait = 0.0f;   // s pendant lesquelles on attend la fin du rechargement
static float switch_wait = 0.0f;   // s après un changement d'arme
static float strafe_t = 0.0f;      // temps restant du décalage latéral
static int   strafe_dir = 1;

// Détection de blocage : si on veut avancer mais qu'on ne bouge presque
// pas, on est coincé contre un obstacle -> saut + pas de côté
static V3    last_pos;
static uint32_t last_pos_tick = 0; // tick de last_pos : on mesure entre deux snapshots
static float stuck_time = 0.0f;    // depuis combien de temps on est coincé
static float dodge_time = 0.0f;    // temps restant de pas de côté
static int   dodge_side = 1;       // -1 = gauche, +1 = droite

// Chemin en cours sur les points de passage (indices dans NAV_NODES)
#define PATH_MAX 64
static int   path[PATH_MAX];
static int   path_len = 0, path_i = 0;
static float path_age = 99.0f;     // secondes depuis le dernier calcul

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

    // Accepte une IP comme un NOM DNS, et un port colle a l'hote
    // (« jeu.exemple.fr:27015 ») ; sans port, DEFAULT_PORT s'applique.
    if (!ResolveServerAddr(ip, DEFAULT_PORT, &server_addr)) {
        printf("Serveur introuvable : %s\n", ip);
        return false;
    }

    PacketConnect pkt = {0};
    pkt.type = PKT_CL_CONNECT;
    pkt.version = PROTO_VERSION;
    pkt.is_bot = (uint8_t)kind;    // poussin zombie ou soldat (dessin, équipement)
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
                printf("[bot] connecté : id %d, équipe %s\n", my_id,
                       my_team == TEAM_RED ? "ROUGE" : "BLEUE");
                return true;
            }
            if (n >= (sock_ssize_t)sizeof(PacketHeader) && buf[0] == PKT_SV_REJECT) {
                printf("[bot] connexion refusée\n");
                return false;
            }
            PlatformSleepMs(10);
        }
        printf("[bot] connexion à %s... (%d/10)\n", ip, attempt + 1);
    }
    printf("[bot] serveur injoignable\n");
    return false;
}

// Ajoute l'entrée de ce tick (déplacement relatif à yaw : avant = vers la
// proie), en ne gardant que les INPUT_BATCH dernières
static void PushInput(float dt, float mx, float my, bool jump)
{
    if (pending_n == INPUT_BATCH) {
        memmove(&pending[0], &pending[1], sizeof(InputEntry) * (INPUT_BATCH - 1));
        pending_n--;
    }
    InputEntry *e = &pending[pending_n++];
    memset(e, 0, sizeof(*e));
    e->seq = ++my_seq;
    e->dt = (dt > INPUT_MAX_DT) ? INPUT_MAX_DT : dt;
    e->move_x = (int8_t)lroundf(mx * 127.0f);
    e->move_y = (int8_t)lroundf(my * 127.0f);
    e->flags = (uint8_t)((jump ? INPUT_JUMP : 0) | (use_held ? INPUT_USE : 0));
    e->yaw = yaw;
    e->pitch = pitch;              // un zombie regarde droit devant, un soldat vise
}

// Envoie les dernières entrées (vide = simple battement de cœur)
static void NetSendInputs(void)
{
    PacketClientInput pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PKT_CL_INPUT;
    pkt.player_id = (uint8_t)my_id;
    pkt.spawn_count = spawn_count;
    pkt.count = (uint8_t)pending_n;
    memcpy(pkt.entries, pending, sizeof(InputEntry) * (size_t)pending_n);
    sendto(sock_fd, &pkt, sizeof(pkt), 0,
           (struct sockaddr *)&server_addr, sizeof(server_addr));
}

static void NetSendAttack(V3 origin, V3 dir)
{
    int slot = (has_snap && snap.players[my_id].active) ? snap.players[my_id].slot : SLOT_PRIMARY;
    PacketShoot pkt = {
        PKT_CL_SHOOT, (uint8_t)my_id,
        (uint8_t)slot, 0, ++shot_seq,       // l'arme en main (les griffes sont la « principale »)
        origin.x, origin.y, origin.z,
        dir.x, dir.y, dir.z,
        (float)pos_tick / SERVER_TICK_HZ,   // on vise ce que montrait le dernier snapshot
    };
    sendto(sock_fd, &pkt, sizeof(pkt), 0,
           (struct sockaddr *)&server_addr, sizeof(server_addr));
}

// Équipement, changement d'arme, rechargement (soldat)
static void NetSendWeapon(int action, int a, int b)
{
    PacketWeapon pkt = { PKT_CL_WEAPON, (uint8_t)my_id, (uint8_t)action, (uint8_t)a, (uint8_t)b };
    sendto(sock_fd, &pkt, sizeof(pkt), 0,
           (struct sockaddr *)&server_addr, sizeof(server_addr));
}

// Lit tous les paquets en attente. On ne garde que les snapshots : les
// événements (kills...) ne servent à rien à un bot.
static void NetReceive(void)
{
    for (;;) {
        uint8_t buf[4096];
        sock_ssize_t n = recvfrom(sock_fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n <= 0) break;

        if (buf[0] == PKT_SV_SNAPSHOT && n >= (sock_ssize_t)sizeof(PacketSnapshot)) {
            memcpy(&snap, buf, sizeof(snap));
            has_snap = true;

            // Le serveur simule notre déplacement : notre position est
            // TOUJOURS celle qu'il nous donne (50 ms de retard, sans
            // importance pour un zombie). Au respawn on écho son compteur
            // dans nos paquets d'entrées et on oublie les entrées d'avant.
            SnapshotPlayer *me = &snap.players[my_id];
            if (me->active) {
                pos = (V3){me->x, me->y, me->z};
                pos_tick = snap.tick;
                if (me->spawn_count != spawn_count) {
                    spawn_count = me->spawn_count;
                    pending_n = 0;
                    last_pos = pos;
                    last_pos_tick = pos_tick;
                    stuck_time = 0.0f;
                    printf("[bot] respawn en (%.0f, %.0f)\n", pos.x, pos.z);
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

//----------------------------------------------------------------------------------
// Points de passage
//----------------------------------------------------------------------------------
// Voit-on b depuis a (à hauteur d'yeux, à travers la carte) ?
static bool CanSee(V3 a, V3 b)
{
    V3 o = {a.x, a.y + PLAYER_EYE_HEIGHT, a.z};
    V3 d = {b.x - a.x, (b.y + 1.0f) - o.y, b.z - a.z};
    float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len < 0.01f) return true;
    d.x /= len; d.y /= len; d.z /= len;
    return RaycastMap(o, d) >= len - 0.05f;
}

// Peut-on MARCHER droit de a à b ? Voir ne suffit pas : une caisse d'un
// mètre ou des sacs de sable se voient par-dessus mais ne se passent pas,
// et une passerelle se quitte par ses escaliers. On fait donc MARCHER un
// joueur simulé (PlayerStep, la même physique que le serveur) tout droit
// vers b, et on regarde s'il arrive. Une dizaine de millisecondes de calcul
// au plus : à n'appeler que quand on recalcule un chemin, pas à chaque tick.
static bool CanWalk(V3 a, V3 b)
{
    float dx = b.x - a.x, dz = b.z - a.z, len = sqrtf(dx * dx + dz * dz);
    if (len < 0.5f) return true;
    PlayerSim s;
    memset(&s, 0, sizeof(s));
    s.pos = a;
    s.on_ground = true;
    int ticks = (int)((len / PLAYER_SPEED + 1.0f) * 60);
    if (ticks > 900) ticks = 900;
    for (int i = 0; i < ticks; i++) {
        PlayerCmd c = {0};
        c.move_y = 1.0f;
        c.speed_mult = 1.0f;
        c.yaw = atan2f(b.x - s.pos.x, b.z - s.pos.z);
        PlayerStep(&s, &c, 1.0f / 60.0f);
        float ex = b.x - s.pos.x, ez = b.z - s.pos.z;
        if (ex * ex + ez * ez < 1.5f * 1.5f) return true;
    }
    return false;
}

// Vers `goal` tout droit, si on peut y marcher : vérifié toutes les
// 0,7 s seulement (CanWalk simule une marche entière)
static bool DirectWalk(V3 goal, float dt)
{
    static float age = 99.0f;
    static bool  ok = false;
    static V3    last_goal;
    age += dt;
    float gx = goal.x - last_goal.x, gz = goal.z - last_goal.z;
    if (age > 0.7f || gx * gx + gz * gz > 4.0f) {
        age = 0.0f;
        last_goal = goal;
        ok = CanWalk(pos, goal);
    }
    return ok;
}

// Nœud le plus proche de p, visible depuis p si possible
static int NearestNode(V3 p)
{
    int best = 0, best_vis = -1;
    float bd = 1e9f, bdv = 1e9f;
    for (int i = 0; i < NAV_NODE_COUNT; i++) {
        float dx = NAV_NODES[i].pos.x - p.x, dz = NAV_NODES[i].pos.z - p.z;
        float d = dx * dx + dz * dz;
        if (d < bd) { bd = d; best = i; }
        if (d < bdv && CanSee(p, NAV_NODES[i].pos)) { bdv = d; best_vis = i; }
    }
    return best_vis >= 0 ? best_vis : best;
}

// Chemin le plus court (en étapes) de from à to : parcours en largeur
static void FindPath(int from, int to)
{
    int prev[64], queue[64], head = 0, tail = 0;
    for (int i = 0; i < NAV_NODE_COUNT && i < 64; i++) prev[i] = -2;
    prev[from] = -1;
    queue[tail++] = from;
    while (head < tail && prev[to] == -2) {
        int n = queue[head++];
        for (int k = 0; k < 6 && NAV_NODES[n].links[k] >= 0; k++) {
            int j = NAV_NODES[n].links[k];
            if (j < 64 && prev[j] == -2) { prev[j] = n; queue[tail++] = j; }
        }
    }
    path_len = 0;
    if (prev[to] == -2) return;                 // inatteignable
    int tmp[PATH_MAX], n = 0;
    for (int c = to; c != -1 && n < PATH_MAX; c = prev[c]) tmp[n++] = c;
    for (int i = n - 1; i >= 0; i--) path[path_len++] = tmp[i];   // dans l'ordre
    path_i = 0;
}

static V3 Steer(V3 goal, bool direct, float dt);   // défini avec le soldat, partagé

// Un pas de simulation du zombie (appelé ZOMBIE_TICK_HZ fois par seconde)
static void ZombieThink(float dt)
{
    if (attack_cd > 0.0f) attack_cd -= dt;
    if (jump_hold > 0.0f) jump_hold -= dt;

    // Mort, pas encore de snapshot, ou match fini : une entrée vide quand
    // même (elle fait tourner la gravité et sert de battement de cœur)
    if (!alive || !has_snap || snap.match_over) { PushInput(dt, 0, 0, false); return; }

    float dist;
    SnapshotPlayer *target = NearestEnemy(&dist);
    if (!target) { PushInput(dt, 0, 0, false); return; }   // personne à chasser : planté là

    // --- Où aller : droit sur la proie si on la voit, sinon vers le
    // prochain point de passage du chemin qui y mène. ---
    V3 tpos = {target->x, target->y, target->z};
    bool sees = CanSee(pos, tpos);
    V3 goal = Steer(tpos, sees, dt);   // droit sur elle s'il la voit, sinon de nœud en nœud

    // Il regarde sa proie s'il la voit, sinon là où il va : le déplacement
    // est relatif à yaw, donc « en avant » = vers son but
    V3 to_goal = {goal.x - pos.x, 0, goal.z - pos.z};
    float glen = sqrtf(to_goal.x * to_goal.x + to_goal.z * to_goal.z);
    if (glen > 0.001f) { to_goal.x /= glen; to_goal.z /= glen; }
    yaw = atan2f(to_goal.x, to_goal.z);

    // --- Déplacement : vers le but, sauf pendant un pas de côté
    // (anti-blocage). Le serveur applique PLAYER_SPEED x longueur du
    // déplacement : on envoie ZOMBIE_SPEED / PLAYER_SPEED pour rester lent.
    // On s'arrête un peu avant le contact pour ne pas pousser la cible. ---
    float mx = 0.0f, my = 0.0f;
    if (!sees || dist > ZOMBIE_ATTACK_RANGE * 0.7f) {
        float k = ZOMBIE_SPEED / PLAYER_SPEED;
        if (dodge_time > 0.0f) { dodge_time -= dt; mx = k * (float)dodge_side; }   // pas de côté
        else my = k;
    }

    // --- Anti-blocage : la position ne change qu'à l'arrivée d'un snapshot,
    // on mesure donc entre deux snapshots. Si on voulait avancer mais qu'on
    // n'a presque pas bougé pendant 0,4 s : saut (obstacle bas) et pas de
    // côté d'une demi-seconde dans un sens aléatoire. ---
    if (last_pos_tick == 0) { last_pos = pos; last_pos_tick = pos_tick; }
    if (pos_tick != last_pos_tick) {
        float span = (float)(pos_tick - last_pos_tick) / SERVER_TICK_HZ;
        float moved = fabsf(pos.x - last_pos.x) + fabsf(pos.z - last_pos.z);
        bool wants_to_move = (mx != 0.0f || my != 0.0f);
        if (wants_to_move && moved < ZOMBIE_SPEED * span * 0.3f) {
            stuck_time += span;
            if (stuck_time > 0.4f) {
                stuck_time = 0.0f;
                jump_hold = 0.15f;          // le serveur ne saute que s'il est au sol
                dodge_time = 0.5f;
                dodge_side = (rand() % 2) ? 1 : -1;
            }
        } else {
            stuck_time = 0.0f;
        }
        last_pos = pos;
        last_pos_tick = pos_tick;
    }

    PushInput(dt, mx, my, jump_hold > 0.0f);

    // --- Griffe : à portée et cooldown écoulé. L'attaque part de nos
    // yeux vers la poitrine de la cible (comme un tir à bout portant,
    // c'est le serveur qui vérifie et applique les dégâts). ---
    if (sees && dist < ZOMBIE_ATTACK_RANGE && attack_cd <= 0.0f) {
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
// Cerveau du soldat
//----------------------------------------------------------------------------------
static float Randf(float a, float b) { return a + (b - a) * (float)rand() / (float)RAND_MAX; }

// Distance de combat préférée selon l'arme en main
static float PreferredRange(int weapon)
{
    switch (weapon) {
    case WPN_BENELLI: return 7.0f;
    case WPN_MP5:     return 14.0f;
    case WPN_GLOCK:
    case WPN_PAMAS:   return 12.0f;
    case WPN_FRF2:    return 35.0f;
    default:          return 20.0f;
    }
}

// Avance le chemin vers goal (points de passage) et rend le point à viser
// pour ce tick : goal lui-même s'il est visible, sinon le prochain nœud.
static V3 Steer(V3 goal, bool direct, float dt)
{
    path_age += dt;
    if (direct) { path_len = 0; return goal; }
    if (path_age > 1.0f || path_len == 0) {
        path_age = 0.0f;
        FindPath(NearestNode(pos), NearestNode(goal));
        // Le nœud le plus proche est parfois DERRIÈRE nous : on saute tous
        // ceux dont on peut déjà rejoindre le suivant en marchant droit
        while (path_i + 1 < path_len && CanWalk(pos, NAV_NODES[path[path_i + 1]].pos)) path_i++;
        // Et si le but lui-même est à portée de marche, plus besoin de nœuds
        if (path_len > 0 && CanWalk(pos, goal)) path_len = 0;
    }
    while (path_i < path_len) {
        V3 n = NAV_NODES[path[path_i]].pos;
        float dx = n.x - pos.x, dz = n.z - pos.z;
        if (dx * dx + dz * dz < 2.5f * 2.5f) { path_i++; continue; }
        break;
    }
    if (path_i < path_len) return NAV_NODES[path[path_i]].pos;
    return goal;
}

// Déplacement MONDE voulu -> commande relative à yaw (avant / droite)
static void WorldToMove(V3 dir_world, float k, float *mx, float *my)
{
    float fx = sinf(yaw), fz = cosf(yaw);       // avant
    float rx = fz, rz = -fx;                    // droite
    *my = (dir_world.x * fx + dir_world.z * fz) * k;
    *mx = (dir_world.x * rx + dir_world.z * rz) * k;
}

// Anti-blocage partagé : si on voulait avancer et qu'on n'a presque pas
// bougé entre deux snapshots, saut et pas de côté.
static void StuckCheck(float mx, float my, float speed)
{
    if (last_pos_tick == 0) { last_pos = pos; last_pos_tick = pos_tick; }
    if (pos_tick == last_pos_tick) return;
    float span = (float)(pos_tick - last_pos_tick) / SERVER_TICK_HZ;
    float moved = fabsf(pos.x - last_pos.x) + fabsf(pos.z - last_pos.z);
    bool wants_to_move = (mx != 0.0f || my != 0.0f);
    if (wants_to_move && moved < speed * span * 0.3f) {
        stuck_time += span;
        if (stuck_time > 0.4f) {
            stuck_time = 0.0f;
            jump_hold = 0.15f;
            dodge_time = 0.5f;
            dodge_side = (rand() % 2) ? 1 : -1;
        }
    } else {
        stuck_time = 0.0f;
    }
    last_pos = pos;
    last_pos_tick = pos_tick;
}

static void SoldierThink(float dt)
{
    if (jump_hold > 0.0f) jump_hold -= dt;
    if (fire_pause > 0.0f) fire_pause -= dt;
    if (reload_wait > 0.0f) reload_wait -= dt;
    if (switch_wait > 0.0f) switch_wait -= dt;
    if (strafe_t > 0.0f) strafe_t -= dt;
    use_held = false;

    if (!alive || !has_snap || snap.match_over) { PushInput(dt, 0, 0, false); return; }
    SnapshotPlayer *me = &snap.players[my_id];
    int slot = me->slot < WEAPON_SLOTS ? me->slot : 0;
    const WeaponStats *w = &WEAPONS[me->weapon[slot]];

    float dist = 1e9f;
    SnapshotPlayer *target = NearestEnemy(&dist);
    V3 tpos = target ? (V3){target->x, target->y, target->z} : pos;
    bool sees = target && dist < SOLDIER_SEE_RANGE && CanSee(pos, tpos);

    // --- La bombe : porter, poser, ramasser, désamorcer ---
    bool carrier = (snap.bomb_state == BOMB_CARRIED && snap.bomb_carrier == my_id);
    bool defuser = (my_team != BOMB_TEAM && snap.bomb_state == BOMB_PLANTED);
    bool fetch   = (my_team == BOMB_TEAM && snap.bomb_state == BOMB_DROPPED);
    V3 bomb = {snap.bomb_x, snap.bomb_y, snap.bomb_z};
    bool fighting = sees && dist < (carrier || defuser ? 18.0f : SOLDIER_SEE_RANGE);

    V3 goal = pos;
    bool direct = false, stand = false;
    float k = 1.0f;
    if (carrier && !fighting) {
        // Vers le site le plus proche ; une fois dessus, immobile et action maintenue
        if (BombSiteAt(pos) >= 0) { stand = true; use_held = true; }
        else {
            int best = 0; float bd = 1e9f;
            for (int i = 0; i < BOMB_SITE_COUNT; i++) {
                V3 n = NAV_NODES[BOMB_SITE_NODE[i]].pos;
                float d = (n.x - pos.x) * (n.x - pos.x) + (n.z - pos.z) * (n.z - pos.z);
                if (d < bd) { bd = d; best = i; }
            }
            goal = NAV_NODES[BOMB_SITE_NODE[best]].pos;
        }
    } else if ((defuser || fetch) && !fighting) {
        float dx = bomb.x - pos.x, dz = bomb.z - pos.z;
        if (dx * dx + dz * dz < (BOMB_USE_RANGE * 0.7f) * (BOMB_USE_RANGE * 0.7f)) {
            stand = true;
            use_held = defuser;
        } else {
            goal = bomb;
            direct = DirectWalk(bomb, dt);
        }
    } else if (fighting) {
        // Garder la distance de son arme : avancer, reculer, ou se décaler
        float pref = PreferredRange(me->weapon[slot]);
        k = SOLDIER_FIGHT_K;
        if (dist > pref + 5.0f) { goal = tpos; direct = true; }
        else if (dist < pref - 4.0f) {
            // Reculer : le point opposé à la cible
            goal = (V3){pos.x - (tpos.x - pos.x), pos.y, pos.z - (tpos.z - pos.z)};
            direct = true;
        } else {
            if (strafe_t <= 0.0f) { strafe_t = Randf(0.5f, 1.2f); strafe_dir = (rand() % 2) ? 1 : -1; }
            // Se décaler perpendiculairement à la ligne de tir
            V3 to = {tpos.x - pos.x, 0, tpos.z - pos.z};
            goal = (V3){pos.x + to.z * strafe_dir, pos.y, pos.z - to.x * strafe_dir};
            direct = true;
        }
    } else if (target) {
        goal = tpos;
    } else {
        stand = true;
    }

    // --- Orientation : vers la cible si on la voit, sinon vers où on va ---
    V3 aim_dir = {0, 0, 1};
    if (sees) {
        V3 eye = {pos.x, pos.y + PLAYER_EYE_HEIGHT, pos.z};
        V3 chest = {tpos.x, tpos.y + 1.1f, tpos.z};
        aim_dir = (V3){chest.x - eye.x, chest.y - eye.y, chest.z - eye.z};
        float l = sqrtf(aim_dir.x * aim_dir.x + aim_dir.y * aim_dir.y + aim_dir.z * aim_dir.z);
        if (l > 0.001f) { aim_dir.x /= l; aim_dir.y /= l; aim_dir.z /= l; }
        yaw = atan2f(aim_dir.x, aim_dir.z);
        pitch = asinf(aim_dir.y);
    } else {
        V3 steer = stand ? pos : Steer(goal, direct, dt);
        V3 to = {steer.x - pos.x, 0, steer.z - pos.z};
        float l = sqrtf(to.x * to.x + to.z * to.z);
        if (l > 0.001f) yaw = atan2f(to.x, to.z);
        pitch += (0.0f - pitch) * fminf(1.0f, dt * 4.0f);
    }

    // --- Déplacement ---
    float mx = 0.0f, my = 0.0f;
    if (!stand) {
        V3 steer = sees ? goal : Steer(goal, direct, dt);
        V3 to = {steer.x - pos.x, 0, steer.z - pos.z};
        float l = sqrtf(to.x * to.x + to.z * to.z);
        if (l > 0.3f) {
            to.x /= l; to.z /= l;
            WorldToMove(to, k, &mx, &my);
        }
        if (dodge_time > 0.0f) { dodge_time -= dt; mx = k * (float)dodge_side; my = 0; }
    }
    StuckCheck(mx, my, PLAYER_SPEED * k);
    PushInput(dt, mx, my, jump_hold > 0.0f);

    // Journal de débogage (EMA_BOT_DEBUG=1) : une ligne par seconde
    if (debug) {
        static float dbg_t = 0.0f;
        dbg_t += dt;
        if (dbg_t >= 1.0f) {
            dbg_t = 0.0f;
            V3 st = (path_i < path_len) ? NAV_NODES[path[path_i]].pos : goal;
            printf("[dbg] pos (%.1f,%.2f,%.1f) bombe etat %d a (%.0f,%.0f) porteur %d | carrier %d defuser %d fetch %d fighting %d stand %d use %d | but (%.0f,%.0f) vers (%.0f,%.0f) direct %d chemin %d/%d | cible %s dist %.0f sees %d | mx %.1f my %.1f saut %d\n",
                   pos.x, pos.y, pos.z, snap.bomb_state, snap.bomb_x, snap.bomb_z, snap.bomb_carrier,
                   carrier, defuser, fetch, fighting, stand, use_held, goal.x, goal.z, st.x, st.z, direct, path_i, path_len,
                   target ? target->name : "-", dist, sees, mx, my, jump_hold > 0.0f);
        }
    }

    // --- Munitions : recharger à vide, passer au pistolet sans réserve ---
    if (w->mag > 0 && me->mag[slot] == 0 && reload_wait <= 0.0f && switch_wait <= 0.0f) {
        if (me->reserve[slot] > 0) {
            NetSendWeapon(WA_RELOAD, 0, 0);
            reload_wait = w->reload + 0.3f;
        } else {
            int other = (slot == SLOT_PRIMARY) ? SLOT_SECONDARY : SLOT_PRIMARY;
            if (me->mag[other] > 0 || me->reserve[other] > 0) {
                NetSendWeapon(WA_SWITCH, other, 0);
                switch_wait = WEAPON_SWITCH_TIME + 0.2f;
            }
        }
        return;
    }
    if (me->reload_t > 0.0f || reload_wait > 0.0f || switch_wait > 0.0f) return;

    // --- Tir : à vue, à portée, selon le mode de l'arme ---
    if (!sees || dist > w->range || fire_pause > 0.0f || !(w->mag == 0 || me->mag[slot] > 0)) return;
    bool shoot = false;
    if (w->mode == FIRE_AUTO) {
        if (burst_left <= 0) { burst_left = 3 + rand() % 4; }
        burst_left--;
        shoot = true;
        fire_pause = (burst_left <= 0) ? Randf(0.35f, 0.7f) : w->cooldown;
    } else {
        shoot = true;
        fire_pause = fmaxf(w->cooldown, 0.25f) + Randf(0.0f, 0.2f);
    }
    if (shoot) {
        // Erreur de visée : un cône dont l'ouverture grandit avec la
        // distance et rétrécit avec l'adresse
        float err = (0.012f + dist * 0.0006f) * (float)(4 - skill) / 2.0f;
        V3 r = {aim_dir.z, 0, -aim_dir.x};
        float rl = sqrtf(r.x * r.x + r.z * r.z);
        if (rl > 1e-4f) { r.x /= rl; r.z /= rl; } else r = (V3){1, 0, 0};
        V3 u = {r.y * aim_dir.z - r.z * aim_dir.y, r.z * aim_dir.x - r.x * aim_dir.z, r.x * aim_dir.y - r.y * aim_dir.x};
        float a = Randf(-err, err), b = Randf(-err, err);
        V3 d = {aim_dir.x + r.x * a + u.x * b, aim_dir.y + r.y * a + u.y * b, aim_dir.z + r.z * a + u.z * b};
        float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        d.x /= l; d.y /= l; d.z /= l;
        V3 eye = {pos.x, pos.y + PLAYER_EYE_HEIGHT, pos.z};
        NetSendAttack(eye, d);
    }
}

//----------------------------------------------------------------------------------
// Boucle principale : simulation à 60 Hz, envoi d'état à 30 Hz
//----------------------------------------------------------------------------------
int main(int argc, char **argv)
{
    const char *ip = (argc > 1) ? argv[1] : "127.0.0.1";
    const char *name = (argc > 2) ? argv[2] : "Zombie";
    if (argc > 3 && (argv[3][0] == 's' || argv[3][0] == 'S')) kind = BOT_SOLDIER;
    if (argc > 4) { skill = atoi(argv[4]); if (skill < 0) skill = 0; if (skill > 3) skill = 3; }
    srand((unsigned)(time(NULL) ^ getpid()));
    setvbuf(stdout, NULL, _IOLBF, 0);   // logs visibles même redirigés
    debug = getenv("EMA_BOT_DEBUG") != NULL;
    MapInit();

    if (!NetConnect(ip, name)) return 1;
    if (kind == BOT_SOLDIER) {
        // Une arme au hasard, et un pistolet
        int primary = WPN_FAMAS + rand() % (WPN_BENELLI - WPN_FAMAS + 1);
        int secondary = (rand() % 2) ? WPN_GLOCK : WPN_PAMAS;
        NetSendWeapon(WA_LOADOUT, primary, secondary);
        printf("[soldat] %s + %s, adresse %d\n", WEAPONS[primary].name, WEAPONS[secondary].name, skill);
    }

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
            if (kind == BOT_SOLDIER) SoldierThink(dt);
            else ZombieThink(dt);
        }

        // L'envoi des entrées sert aussi de battement de cœur : sans lui le
        // serveur nous déconnecterait au timeout de 5 s
        if (now - last_send >= send_dt) {
            last_send = now;
            NetSendInputs();
        }

        PlatformSleepMs(2);   // ~2 ms : ne sature pas un cœur de CPU
    }
    return 0;
}
