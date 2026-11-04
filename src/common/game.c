// game.c — Map et collisions partagées client/serveur
#include <math.h>
#include <float.h>
#include "game.h"

// --- La carte -------------------------------------------------------------
// Inspirée de Dust 2 (Counter-Strike), à l'échelle d'une vraie carte :
// 120 x 120 m, faite de boîtes alignées sur les axes. Les T (bleus)
// partent au SUD (z > 0), les CT (rouges) au NORD (z < 0). Comme dans
// l'original, trois voies relient les deux camps, et deux SITES (A au
// nord-est, B au nord-ouest) sont les points de rencontre :
//
//      x : -60 ........ -34 ..... -10 ... 0 ... 10 ..... 34 ........ 60
//  z -60  +---------------------------------------------------------+
//         | SITE B         | B doors |  CT SPAWN   |      | SITE A  |
//  z -42  |      (plateau) +---------+             +------+         |
//         |        connecteur ------>|  top mid    | N1   | (plateau)|
//  z -24  +----------------+---------+   doubles   +------+---------+
//         |  tunnels       |          |   portes   | catwalk ---->  |
//         |  hauts    bloc ouest      |            |    bloc est    |
//  z   2  |  (toit)  -- tunnels bas (toit) ------> |  MID           |
//  z   8  |                           |  enseigne  |            long|
//         |                bloc ouest |  caisse    |    bloc est   A|
//  z  32  |                           |            +-----------+    |
//  z  40  +----------------+----------+            | outside   doors|
//         |                   T SPAWN                                 |
//  z  60  +---------------------------------------------------------+
//
//   MID       : couloir central de 20 m de large, coupé par les « doubles
//               portes » (deux piliers, passage de 6 m) ; au sud l'enseigne
//               au poussin et la caisse « xbox » servent de couverture.
//   LONG A    : à l'est, un couloir de 12 m de large et 58 m de long,
//               fermé au sud par les « long doors » (passage de 4 m), qui
//               débouche au nord sur le site A.
//   CATWALK   : du haut de mid vers le site A, un couloir de 6 m.
//   TUNNELS   : à l'ouest, les tunnels HAUTS longent l'enceinte sous un
//               toit (il laisse passer un joueur mais bloque les tirs en
//               cloche) jusqu'au site B ; les tunnels BAS les relient à mid
//               à travers le bloc ouest, sous un toit eux aussi.
//   CONNECTEUR: du haut de mid vers le site B, un couloir de 6 m.
//   B DOORS   : du spawn CT au site B, une porte de 4 m.
//   Les sites ont un plateau à 1,60 m (deux marches de 0,80 m, un saut
//   monte à 1 m) et des caisses.
// Les voies font au moins 4 m de large : les bots suivent des points de
// passage (NAV_NODES) et doivent pouvoir marcher droit de l'un à l'autre.
#define H_WALL  6.0f     // hauteur des blocs et de l'enceinte : infranchissable
#define H_DOOR  4.0f     // piliers des portes
#define H_ROOF0 3.2f     // dessous des toits : un joueur (1,80 m) passe
#define H_ROOF1 4.0f
const Box MAP_BOXES[] = {
    // --- Enceinte ---
    {{-ARENA_HALF - 1, 0, -ARENA_HALF - 1}, { ARENA_HALF + 1, H_WALL, -ARENA_HALF}},  // nord
    {{-ARENA_HALF - 1, 0,  ARENA_HALF},     { ARENA_HALF + 1, H_WALL,  ARENA_HALF + 1}}, // sud
    {{-ARENA_HALF - 1, 0, -ARENA_HALF},     {-ARENA_HALF, H_WALL,  ARENA_HALF}},  // ouest
    {{ ARENA_HALF, 0, -ARENA_HALF},         { ARENA_HALF + 1, H_WALL, ARENA_HALF}}, // est

    // --- Bloc ouest (entre les tunnels et mid), coupé par les tunnels bas (z 2..8) ---
    {{-46, 0, -24}, {-10, H_WALL,  2}},
    {{-46, 0,   8}, {-10, H_WALL, 40}},
    // Toit des tunnels bas
    {{-46, H_ROOF0, 2}, {-10, H_ROOF1, 8}},
    // Toit des tunnels hauts (le long de l'enceinte ouest)
    {{-ARENA_HALF, H_ROOF0, -20}, {-46, H_ROOF1, 30}},

    // --- Bloc nord-ouest (entre le connecteur et le passage CT -> B) ---
    {{-34, 0, -42}, {-10, H_WALL, -30}},
    // B doors : mur avec un passage de 4 m (z -46..-42) vers le site B
    {{-36, 0, -58}, {-34, H_WALL, -46}},

    // --- Bloc nord-est (entre top mid et le site A) ---
    {{18, 0, -42}, {34, H_WALL, -24}},

    // --- Bloc est (entre mid et long A) ---
    {{10, 0, -18}, {34, H_WALL, 32}},
    {{34, 0, -18}, {46, H_WALL, 36}},
    // Long doors : mur avec un passage de 4 m (x 50..54)
    {{46, 0, 34}, {50, H_WALL, 36}},
    {{54, 0, 34}, {58, H_WALL, 36}},

    // --- Mid : les doubles portes (passage de 6 m), l'enseigne, la caisse ---
    {{-10, 0, -14}, {-3, H_DOOR, -12}},
    {{  3, 0, -14}, {10, H_DOOR, -12}},
    {{-1.5f, 0, -1.5f}, {1.5f, 3, 1.5f}},     // pilier de l'enseigne
    {{-1.5f, 0, 14.5f}, {1.5f, 2, 17.5f}},    // « xbox »

    // --- Site A (nord-est) : plateau à 1,60 m avec sa marche, caisses ---
    {{36, 0, -56}, {40, 0.8f, -48}},
    {{40, 0, -56}, {52, 1.6f, -48}},
    {{48, 0, -34}, {50, 2, -32}},
    {{37, 0, -30}, {40, 1.5f, -27}},
    {{54, 0, -40}, {56, 1.2f, -37}},

    // --- Site B (nord-ouest) : plateau à 1,60 m, caisses ---
    {{-46, 0, -56}, {-42, 0.8f, -52}},
    {{-56, 0, -56}, {-46, 1.6f, -50}},
    {{-50, 0, -39}, {-48, 2, -37}},
    {{-57, 0, -34}, {-55, 1.5f, -31}},
    {{-56, 0, -44}, {-54, 1.2f, -41}},

    // --- Couvertures basses : spawns, catwalk, long, CT -> A ---
    {{-22, 0, 54}, {-19, 1.2f, 57}},      // T spawn
    {{ 18, 0, 54}, { 21, 1.2f, 57}},
    {{-16, 0, -47}, {-13, 1.2f, -44}},    // CT spawn
    {{ 13, 0, -47}, { 16, 1.2f, -44}},
    {{ 30, 0, -19.5f}, { 32, 1.0f, -18}}, // catwalk
    {{ 55, 0, 18}, { 57, 2, 21}},         // long A
    {{ 30, 0, -57}, { 32, 1.2f, -55}},    // CT -> A
    {{ 12, 0, -34}, { 15, 1.5f, -31}},    // top mid
    {{-16, 0, -25.5f}, {-14, 1.0f, -24}}, // connecteur
};
const int MAP_BOX_COUNT = (int)(sizeof(MAP_BOXES) / sizeof(MAP_BOXES[0]));

// Spawns : CT (rouges) au nord, T (bleus) au sud, alignés au centre de
// leur camp, face aux trois voies.
const V3 TEAM_SPAWNS[TEAM_COUNT][SPAWNS_PER_TEAM] = {
    // Rouge / CT : au nord (z négatif)
    {{-12, 0, -52}, {-6, 0, -50}, {0, 0, -52}, {6, 0, -50}, {12, 0, -52}},
    // Bleu / T : au sud (z positif)
    {{-12, 0, 52}, {-6, 0, 50}, {0, 0, 52}, {6, 0, 50}, {12, 0, 52}},
};

// Points de passage des bots. Chaque nœud est au milieu d'un couloir ou
// d'une zone ouverte ; deux nœuds liés se voient en ligne droite (le test
// de carte le vérifie).
const NavNode NAV_NODES[] = {
    /* 0  CT spawn        */ {{  0, 0, -50}, {1, 11, 17, -1}},
    /* 1  top mid         */ {{  0, 0, -30}, {0, 2, 7, 16, -1}},
    /* 2  doubles portes  */ {{  0, 0, -13}, {1, 3, -1}},
    /* 3  mid (enseigne)  */ {{ -4, 0,   0}, {2, 4, 15, -1}},
    /* 4  mid bas         */ {{ -4, 0,  28}, {3, 5, -1}},
    /* 5  T spawn         */ {{  0, 0,  50}, {4, 6, 19, -1}},
    /* 6  T spawn ouest   */ {{-40, 0,  50}, {5, 8, -1}},
    /* 7  connecteur      */ {{-22, 0, -27}, {1, 24, -1}},
    /* 8  tunnels, entrée */ {{-52, 0,  34}, {6, 10, -1}},
    /* 9  site B          */ {{-46, 0, -40}, {24, 13, 12, -1}},
    /* 10 tunnels hauts   */ {{-52, 0,   5}, {8, 13, 14, -1}},
    /* 11 CT -> B         */ {{-25, 0, -50}, {0, 12, -1}},
    /* 12 B doors         */ {{-36, 0, -44}, {9, 11, -1}},
    /* 13 tunnels, sortie */ {{-52, 0, -20}, {10, 9, -1}},
    /* 14 tunnels bas     */ {{-28, 0,   5}, {10, 15, -1}},
    /* 15 sortie tun. bas */ {{ -8, 0,   5}, {14, 3, -1}},
    /* 16 catwalk         */ {{ 22, 0, -21}, {1, 23, -1}},
    /* 17 CT -> A         */ {{ 26, 0, -50}, {0, 18, -1}},
    /* 18 site A          */ {{ 46, 0, -38}, {23, 17, 20, -1}},
    /* 19 T spawn est     */ {{ 30, 0,  44}, {5, 21, -1}},
    /* 20 long A          */ {{ 52, 0,  -8}, {18, 22, -1}},
    /* 21 outside long    */ {{ 52, 0,  40}, {19, 22, -1}},
    /* 22 long doors      */ {{ 52, 0,  34}, {20, 21, -1}},
    /* 23 sortie catwalk  */ {{ 38, 0, -21}, {16, 18, -1}},
    /* 24 entrée B (est)  */ {{-38, 0, -27}, {7, 9, -1}},
};
const int NAV_NODE_COUNT = (int)(sizeof(NAV_NODES) / sizeof(NAV_NODES[0]));

// --- Les armes --------------------------------------------------------------
// Dégâts par balle (100 PV), cadence en secondes par coup, portée en mètres.
// Les fusils d'assaut tuent en 4 balles au corps, 2 à la tête ; le FR-F2
// tue d'une balle ; le fusil à pompe tire 8 plombs de 12.
const WeaponStats WEAPONS[WPN_COUNT] = {
    //                 nom           catégorie               mode       dmg  cd      portée mag rés  rechg plombs hanche  visée   recul  vitesse
    [WPN_FAMAS]   = { "FAMAS F1",   "fusil d'assaut",       FIRE_AUTO,  22, 0.060f, 150, 25,  75, 2.6f, 1, 0.020f, 0.0030f, 0.0090f, 1.00f },
    [WPN_HK416]   = { "HK416",      "fusil d'assaut",       FIRE_AUTO,  26, 0.071f, 160, 30,  90, 2.4f, 1, 0.018f, 0.0022f, 0.0100f, 1.00f },
    [WPN_M4A1]    = { "M4A1",       "carabine",             FIRE_AUTO,  25, 0.075f, 150, 30,  90, 2.3f, 1, 0.017f, 0.0022f, 0.0095f, 1.02f },
    [WPN_AK74]    = { "AK-74M",     "fusil d'assaut",       FIRE_AUTO,  31, 0.092f, 150, 30,  90, 2.7f, 1, 0.024f, 0.0030f, 0.0140f, 0.98f },
    [WPN_MP5]     = { "HK MP5",     "pistolet-mitrailleur", FIRE_AUTO,  18, 0.075f,  60, 30, 120, 2.0f, 1, 0.015f, 0.0040f, 0.0060f, 1.08f },
    [WPN_FRF2]    = { "FR-F2",      "fusil de précision",   FIRE_BOLT, 100, 1.500f, 300, 10,  30, 3.2f, 1, 0.060f, 0.0005f, 0.0400f, 0.92f },
    [WPN_BENELLI] = { "Benelli M3", "fusil à pompe",        FIRE_PUMP,  12, 0.900f,  25,  7,  28, 3.0f, 8, 0.045f, 0.0300f, 0.0300f, 1.00f },
    [WPN_GLOCK]   = { "Glock 17",   "pistolet",             FIRE_SEMI,  20, 0.120f,  60, 17,  51, 1.6f, 1, 0.014f, 0.0040f, 0.0120f, 1.10f },
    [WPN_PAMAS]   = { "PAMAS G1",   "pistolet",             FIRE_SEMI,  24, 0.140f,  60, 15,  45, 1.7f, 1, 0.013f, 0.0040f, 0.0140f, 1.10f },
    [WPN_CLAWS]   = { "Griffes",    "poussin zombie",       FIRE_SEMI,  25, 0.900f, 2.5f, 0,   0, 0.0f, 1, 0.000f, 0.0000f, 0.0000f, 1.00f },
};

Box PlayerBox(V3 feet)
{
    Box b;
    b.min = (V3){feet.x - PLAYER_RADIUS, feet.y,                 feet.z - PLAYER_RADIUS};
    b.max = (V3){feet.x + PLAYER_RADIUS, feet.y + PLAYER_HEIGHT, feet.z + PLAYER_RADIUS};
    return b;
}

static bool BoxOverlap(Box a, Box b)
{
    return a.min.x < b.max.x && a.max.x > b.min.x &&
           a.min.y < b.max.y && a.max.y > b.min.y &&
           a.min.z < b.max.z && a.max.z > b.min.z;
}

static bool CollidesMap(V3 feet)
{
    Box pb = PlayerBox(feet);
    for (int i = 0; i < MAP_BOX_COUNT; i++) {
        if (BoxOverlap(pb, MAP_BOXES[i])) return true;
    }
    return false;
}

// Déplace un joueur en résolvant les collisions avec la map.
// Principe : on essaie le déplacement axe par axe (x, puis z, puis y).
// Si la position d'arrivée traverse une boîte, on annule seulement cet
// axe-là : le joueur "glisse" le long du mur au lieu de s'arrêter net.
void MovePlayer(V3 *pos, V3 vel_h, float *vel_y, float dt, bool *on_ground)
{
    // --- Horizontal : axe X puis axe Z, indépendamment ---
    V3 try_pos = *pos;
    try_pos.x += vel_h.x * dt;                  // on tente le pas en X...
    if (!CollidesMap(try_pos)) pos->x = try_pos.x;  // ...accepté si pas de mur

    try_pos = *pos;
    try_pos.z += vel_h.z * dt;                  // pareil pour Z
    if (!CollidesMap(try_pos)) pos->z = try_pos.z;

    // --- Vertical : gravité puis test de l'atterrissage ---
    *vel_y -= GRAVITY * dt;        // la vitesse verticale diminue en continu
    try_pos = *pos;
    try_pos.y += *vel_y * dt;

    *on_ground = false;
    if (try_pos.y <= 0.0f) {            // on touche le sol de l'arène
        try_pos.y = 0.0f;
        *vel_y = 0.0f;
        *on_ground = true;
    } else if (CollidesMap(try_pos)) {  // on rentre dans une boîte par
        if (*vel_y < 0.0f) *on_ground = true;   // le dessus (chute) -> posé
        try_pos.y = pos->y;             // sinon (saut sous un plafond) : stop
        *vel_y = 0.0f;
    }
    pos->y = try_pos.y;

    // On reste dans l'arène (sécurité)
    float lim = ARENA_HALF - PLAYER_RADIUS;
    if (pos->x < -lim) pos->x = -lim;
    if (pos->x >  lim) pos->x =  lim;
    if (pos->z < -lim) pos->z = -lim;
    if (pos->z >  lim) pos->z =  lim;
}

// --- Simulation d'un joueur (partagée) ---------------------------------------
PlayerCmd CmdFromEntry(const InputEntry *e)
{
    PlayerCmd c;
    c.move_x = (float)e->move_x / 127.0f;
    c.move_y = (float)e->move_y / 127.0f;
    c.yaw    = e->yaw;
    c.jump   = (e->flags & INPUT_JUMP) != 0;
    c.sprint = (e->flags & INPUT_SPRINT) != 0;
    c.ads    = (e->flags & INPUT_ADS) != 0;
    c.speed_mult = 1.0f;
    return c;
}

void PlayerStep(PlayerSim *s, const PlayerCmd *c, float dt)
{
    // "devant" et "droite" sont calculés à plat (sans le pitch) : on se
    // déplace au sol même en regardant en l'air.
    V3 fwd = {sinf(c->yaw), 0, cosf(c->yaw)};
    V3 right = {fwd.z, 0, -fwd.x};
    float speed = (c->sprint ? SPRINT_SPEED : PLAYER_SPEED)
                * (1.0f - (c->ads ? 0.4f : 0.0f))    // -40 % en visant
                * (c->speed_mult > 0.0f ? c->speed_mult : 1.0f);   // arme lourde ou légère
    V3 wish = {
        (fwd.x * c->move_y + right.x * c->move_x) * speed, 0,
        (fwd.z * c->move_y + right.z * c->move_x) * speed,
    };
    // La vitesse actuelle se rapproche de la vitesse voulue d'une fraction
    // (dt * accel) par pas : démarrage et freinage en douceur, et faible
    // contrôle en l'air (on garde son élan pendant un saut).
    float accel = s->on_ground ? ACCEL_GROUND : ACCEL_AIR;
    float k = dt * accel;
    if (k > 1.0f) k = 1.0f;
    s->vel.x += (wish.x - s->vel.x) * k;
    s->vel.z += (wish.z - s->vel.z) * k;

    if (c->jump && s->on_ground) s->vel_y = PLAYER_JUMP_SPEED;

    MovePlayer(&s->pos, s->vel, &s->vel_y, dt, &s->on_ground);
}

// Méthode des "slabs" : intersection rayon/AABB
bool RayVsBox(V3 o, V3 d, Box box, float *t_out)
{
    float tmin = 0.0f, tmax = FLT_MAX;
    const float *op = &o.x, *dp = &d.x;
    const float *bmin = &box.min.x, *bmax = &box.max.x;

    for (int i = 0; i < 3; i++) {
        if (fabsf(dp[i]) < 1e-8f) {
            if (op[i] < bmin[i] || op[i] > bmax[i]) return false;
        } else {
            float inv = 1.0f / dp[i];
            float t1 = (bmin[i] - op[i]) * inv;
            float t2 = (bmax[i] - op[i]) * inv;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }
    *t_out = tmin;
    return true;
}

float RaycastMap(V3 origin, V3 dir)
{
    float best = WEAPON_RANGE;
    for (int i = 0; i < MAP_BOX_COUNT; i++) {
        float t;
        if (RayVsBox(origin, dir, MAP_BOXES[i], &t) && t > 0.0f && t < best) {
            best = t;
        }
    }
    // Sol
    if (dir.y < -1e-6f) {
        float t = -origin.y / dir.y;
        if (t > 0.0f && t < best) best = t;
    }
    return best;
}
