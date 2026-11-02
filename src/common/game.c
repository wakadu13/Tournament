// game.c — Map et collisions partagées client/serveur
#include <math.h>
#include <float.h>
#include "game.h"

// Arène : 4 murs d'enceinte + obstacles symétriques (couverture)
const Box MAP_BOXES[MAP_BOX_COUNT] = {
    // Murs d'enceinte (hauteur 4 m)
    {{-ARENA_HALF, 0, -ARENA_HALF - 1}, { ARENA_HALF, 4, -ARENA_HALF}},  // nord
    {{-ARENA_HALF, 0,  ARENA_HALF},     { ARENA_HALF, 4,  ARENA_HALF + 1}}, // sud
    {{-ARENA_HALF - 1, 0, -ARENA_HALF}, {-ARENA_HALF, 4,  ARENA_HALF}},  // ouest
    {{ ARENA_HALF, 0, -ARENA_HALF},     { ARENA_HALF + 1, 4, ARENA_HALF}}, // est

    // Bloc central
    {{-3, 0, -3}, {3, 3, 3}},

    // Couvertures côté rouge (z négatif)
    {{-12, 0, -14}, {-8, 2, -10}},
    {{  8, 0, -14}, { 12, 2, -10}},
    {{ -2, 0, -20}, {  2, 1.2f, -18}},

    // Couvertures côté bleu (z positif)
    {{-12, 0, 10}, {-8, 2, 14}},
    {{  8, 0, 10}, { 12, 2, 14}},
    {{ -2, 0, 18}, {  2, 1.2f, 20}},

    // Couloirs latéraux
    {{-22, 0, -4}, {-18, 2.5f, 4}},
    {{ 18, 0, -4}, { 22, 2.5f, 4}},
};

const V3 TEAM_SPAWNS[TEAM_COUNT][SPAWNS_PER_TEAM] = {
    // Rouge : au nord (z négatif)
    {{-15, 0, -26}, {-5, 0, -26}, {5, 0, -26}, {15, 0, -26}},
    // Bleu : au sud (z positif)
    {{-15, 0,  26}, {-5, 0,  26}, {5, 0,  26}, {15, 0,  26}},
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
