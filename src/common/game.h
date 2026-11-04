// game.h — Règles du jeu, map et collisions partagées client/serveur
#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include "protocol.h"

// --- Gameplay -------------------------------------------------------------
#define PLAYER_MAX_HEALTH   100
#define WEAPON_RANGE        300.0f  // portee MAXIMALE d'un rayon (le FR-F2) : au-dela, rien n'est touche
#define HEADSHOT_HEIGHT     1.5f    // au-dessus de cette hauteur (depuis les pieds) : tir a la tete
#define HEADSHOT_MULT       2.0f    // degats x2 a la tete
#define WEAPON_SWITCH_TIME  0.45f   // secondes pour passer d'une arme a l'autre
#define ROUND_BUY_TIME      20.0f   // debut de manche (s) pendant lequel un nouvel equipement s'applique tout de suite
// Manches à élimination (comme dans Counter-Strike) : pas de respawn
// pendant une manche ; l'équipe qui n'a plus de survivant perd la manche.
#define ROUNDS_TO_WIN       5       // manches pour gagner le match
#define ROUND_TIME          120.0f  // durée maximale d'une manche (s) : au-delà,
                                    // l'équipe avec le plus de survivants l'emporte
#define ROUND_END_DELAY     4.0f    // pause entre deux manches (s)

#define PLAYER_SPEED        7.0f    // m/s
#define SPRINT_SPEED        10.5f   // m/s en sprint
#define ACCEL_GROUND        12.0f   // accélération au sol (réactif)
#define ACCEL_AIR           2.5f    // en l'air : faible contrôle aérien
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

#define ARENA_HALF 60.0f    // l'arène fait 120x120 m, comme une carte de Counter-Strike

extern const Box MAP_BOXES[];
extern const int MAP_BOX_COUNT;

#define SPAWNS_PER_TEAM 5
extern const V3 TEAM_SPAWNS[TEAM_COUNT][SPAWNS_PER_TEAM];

// Points de passage pour les bots : les centres des couloirs et des sites,
// relies entre eux quand on peut marcher droit de l'un a l'autre.
typedef struct { V3 pos; int links[6]; } NavNode;    // links termine par -1
extern const NavNode NAV_NODES[];
extern const int NAV_NODE_COUNT;

// --- Armes ---------------------------------------------------------------
// Une entree par WeaponId (protocol.h), partagee : le serveur applique
// degats, cadence, portee, chargeur et plombs ; le client s'en sert pour la
// dispersion, le recul, le HUD et le menu de choix.
typedef struct {
    const char *name;       // nom affiche
    const char *kind;       // categorie affichee (fusil d'assaut, ...)
    int   mode;             // FireMode
    int   damage;           // par balle ou par plomb
    float cooldown;         // secondes entre deux coups
    float range;            // portee (m) : au-dela, pas de degats
    int   mag;              // cartouches par chargeur (0 = illimite)
    int   reserve;          // cartouches en reserve au spawn
    float reload;           // secondes de rechargement
    int   pellets;          // plombs par coup (1 = une balle)
    float spread_hip;       // dispersion a la hanche (rad) ; les plombs s'y ajoutent
    float spread_ads;       // dispersion en visee (rad)
    float recoil;           // montee du canon par coup (rad)
    float move_mult;        // vitesse de deplacement (1 = normale)
} WeaponStats;
extern const WeaponStats WEAPONS[WPN_COUNT];

static inline int WeaponValidPrimary(int w)   { return w >= WPN_FAMAS && w <= WPN_BENELLI; }
static inline int WeaponValidSecondary(int w) { return w == WPN_GLOCK || w == WPN_PAMAS; }

// --- Collisions -----------------------------------------------------------

// Déplace un joueur (position = pieds) en résolvant les collisions
// avec les murs de l'arène et les boîtes de la map.
// vel_h = vitesse horizontale en m/s (l'appelant gère l'accélération),
// vel_y est mis à jour (gravité/atterrissage), *on_ground indique le sol.
void MovePlayer(V3 *pos, V3 vel_h, float *vel_y, float dt, bool *on_ground);

// --- Simulation d'un joueur, partagée client / serveur / bot ---------------
// Le client la fait tourner en avance (prédiction), le serveur fait
// autorité, et le client se recale sur lui : les deux DOIVENT produire le
// même résultat pour la même entrée. Tout le mouvement passe donc par ici.
typedef struct {
    V3    pos;        // pieds
    V3    vel;        // vitesse horizontale actuelle (x, z ; y inutilisé)
    float vel_y;      // vitesse verticale (saut / gravité)
    bool  on_ground;
} PlayerSim;

// Ce que le joueur demande pendant un pas
typedef struct {
    float move_x, move_y;   // -1..1, longueur <= 1 (droite, avant)
    float yaw;              // orientation de la vue (le déplacement est à plat)
    bool  jump, sprint, ads;
    float speed_mult;       // vitesse de l'arme tenue (WeaponStats.move_mult) ; 0 = 1
} PlayerCmd;

// Une entrée réseau -> commande, en déquantifiant le déplacement EXACTEMENT
// comme le client l'a fait avant de simuler (sinon la prédiction diverge).
PlayerCmd CmdFromEntry(const InputEntry *e);

// Un pas de simulation : vitesse voulue, accélération progressive, saut,
// puis collisions et gravité (MovePlayer).
void PlayerStep(PlayerSim *s, const PlayerCmd *c, float dt);

// Raycast contre une boîte. Retourne true et la distance si touchée.
bool RayVsBox(V3 origin, V3 dir, Box box, float *t_out);

// Raycast contre la map entière (murs + boîtes). Retourne la distance
// du premier impact, ou WEAPON_RANGE si rien n'est touché.
float RaycastMap(V3 origin, V3 dir);

// AABB d'un joueur à partir de la position de ses pieds.
Box PlayerBox(V3 feet_pos);

#endif // GAME_H
