// game.h — Règles du jeu, map et collisions partagées client/serveur
#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include "protocol.h"

// --- Gameplay -------------------------------------------------------------
#define PLAYER_MAX_HEALTH   100
#define WEAPON_DAMAGE       25
#define WEAPON_RANGE        100.0f
#define WEAPON_COOLDOWN     0.15f   // secondes entre deux tirs
#define RESPAWN_DELAY       3.0f    // secondes avant respawn
#define SCORE_LIMIT         30      // kills pour gagner le match

#define PLAYER_SPEED        7.0f    // m/s
#define PLAYER_JUMP_SPEED   6.0f
#define GRAVITY             18.0f
#define PLAYER_RADIUS       0.4f    // demi-largeur du joueur (AABB)
#define PLAYER_HEIGHT       1.8f
#define PLAYER_EYE_HEIGHT   1.6f    // hauteur des yeux depuis les pieds

// --- Vecteur minimal (le serveur ne dépend pas de raylib) ------------------
typedef struct { float x, y, z; } V3;

// --- Map : une arène faite de boîtes alignées sur les axes (AABB) ----------
typedef struct {
    V3 min;
    V3 max;
} Box;

#define ARENA_HALF 30.0f    // l'arène fait 60x60 m

#define MAP_BOX_COUNT 13
extern const Box MAP_BOXES[MAP_BOX_COUNT];

#define SPAWNS_PER_TEAM 4
extern const V3 TEAM_SPAWNS[TEAM_COUNT][SPAWNS_PER_TEAM];

// --- Collisions -----------------------------------------------------------

// Déplace un joueur (position = pieds) en résolvant les collisions
// avec les murs de l'arène et les boîtes de la map.
// vel_h = vitesse horizontale en m/s (l'appelant gère l'accélération),
// vel_y est mis à jour (gravité/atterrissage), *on_ground indique le sol.
void MovePlayer(V3 *pos, V3 vel_h, float *vel_y, float dt, bool *on_ground);

// Raycast contre une boîte. Retourne true et la distance si touchée.
bool RayVsBox(V3 origin, V3 dir, Box box, float *t_out);

// Raycast contre la map entière (murs + boîtes). Retourne la distance
// du premier impact, ou WEAPON_RANGE si rien n'est touché.
float RaycastMap(V3 origin, V3 dir);

// AABB d'un joueur à partir de la position de ses pieds.
Box PlayerBox(V3 feet_pos);

#endif // GAME_H
