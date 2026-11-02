// game.c — Map et collisions partagées client/serveur
#include <math.h>
#include <float.h>
#include "game.h"

// Arène : 4 murs d'enceinte + obstacles symétriques (couverture)
// Carte inspirée de Dust (Counter-Strike), ramenée à une arène de 60 x 60 m
// et à des boîtes alignées sur les axes. Les T (bleus) partent au sud, les
// CT (rouges) au nord. Trois chemins les relient :
//   - MID, au centre : deux piliers « double doors » et le pilier central
//     qui porte l'enseigne, avec un passage de chaque côté ;
//   - LONG A, à l'est : un couloir de 10 m longé d'un mur, qui débouche au
//     nord sur le site A (nord-est) et ses caisses ;
//   - les TUNNELS B, à l'ouest : un couloir étroit et COUVERT (le toit, à
//     3,2 m, ne gêne pas le passage mais bloque les tirs en cloche) qui
//     débouche sur le site B (nord-ouest).
// Les voies restent assez ouvertes pour que les bots, qui marchent droit
// et contournent grossièrement, s'y retrouvent.
const Box MAP_BOXES[MAP_BOX_COUNT] = {
    // Murs d'enceinte (hauteur 4 m)
    {{-ARENA_HALF, 0, -ARENA_HALF - 1}, { ARENA_HALF, 4, -ARENA_HALF}},  // nord
    {{-ARENA_HALF, 0,  ARENA_HALF},     { ARENA_HALF, 4,  ARENA_HALF + 1}}, // sud
    {{-ARENA_HALF - 1, 0, -ARENA_HALF}, {-ARENA_HALF, 4,  ARENA_HALF}},  // ouest
    {{ ARENA_HALF, 0, -ARENA_HALF},     { ARENA_HALF + 1, 4, ARENA_HALF}}, // est

    // MID : les « double doors » (deux piliers) et le pilier de l'enseigne
    {{-8, 0, -1}, {-4, 3, 1}},
    {{ 4, 0, -1}, { 8, 3, 1}},
    {{-1.5f, 0, -1.5f}, {1.5f, 3, 1.5f}},

    // LONG A (est) : le mur qui ferme le couloir côté mid
    {{16, 0, -4}, {18, 3, 18}},

    // TUNNELS B (ouest) : le mur côté mid, et le toit du tunnel
    {{-18, 0, -4}, {-16, 3, 18}},
    {{-28, 3.2f, 0}, {-19, 4, 16}},

    // Site A (nord-est) : caisses
    {{18, 0, -18}, {21, 1.5f, -15}},
    {{22, 0, -14}, {24, 2, -12}},
    {{12, 0, -22}, {15, 2, -19}},

    // Site B (nord-ouest) : caisses
    {{-22, 0, -20}, {-19, 1.5f, -17}},
    {{-16, 0, -14}, {-13, 2, -11}},
    {{-24, 0, -12}, {-21, 2, -10}},

    // Spawn T (sud) et mid : couvertures basses
    {{-8, 0, 22}, {-5, 1.2f, 24}},
    {{ 6, 0, 20}, { 9, 2, 23}},
    {{-10, 0, 8}, {-8, 1.2f, 12}},
    {{ 9, 0, 6}, {11, 1.2f, 9}},
};

// Spawns : CT (rouges) au nord, T (bleus) au sud, groupés au centre de
// leur bord — les trois voies s'ouvrent devant eux.
const V3 TEAM_SPAWNS[TEAM_COUNT][SPAWNS_PER_TEAM] = {
    // Rouge / CT : au nord (z négatif)
    {{-6, 0, -26}, {-2, 0, -26}, {2, 0, -26}, {6, 0, -26}},
    // Bleu / T : au sud (z positif)
    {{-6, 0,  26}, {-2, 0,  26}, {2, 0,  26}, {6, 0,  26}},
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
    return c;
}

void PlayerStep(PlayerSim *s, const PlayerCmd *c, float dt)
{
    // "devant" et "droite" sont calculés à plat (sans le pitch) : on se
    // déplace au sol même en regardant en l'air.
    V3 fwd = {sinf(c->yaw), 0, cosf(c->yaw)};
    V3 right = {fwd.z, 0, -fwd.x};
    float speed = (c->sprint ? SPRINT_SPEED : PLAYER_SPEED)
                * (1.0f - (c->ads ? 0.4f : 0.0f));   // -40 % en visant
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
