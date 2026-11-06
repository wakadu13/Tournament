// game.c — Map et collisions partagées client/serveur
#include <math.h>
#include <float.h>
#include "game.h"

// --- La carte -------------------------------------------------------------
// Inspirée de Dust 2 (Counter-Strike), à l'échelle d'une vraie carte :
// 120 x 120 m. Les T (bleus) partent au SUD (z > 0), les CT (rouges) au
// NORD (z < 0). Trois voies relient les deux camps, et deux SITES (A au
// nord-est, B au nord-ouest) sont les points de rencontre :
//
//      x : -60 ........ -34 ..... -10 ... 0 ... 10 ..... 34 ........ 60
//  z -60  +---------------------------------------------------------+
//         | SITE B         | B doors |  CT SPAWN   |      | SITE A  |
//  z -42  |   (plateau,    +---------+  (arcade)   +------+ (plateau,|
//         |    réduit)  connecteur ->|  top mid    | N1   |  caisses)|
//  z -24  +-------------+-[fenêtre]-+   doubles   +------+---------+
//         |  tunnels    |  salle de |   portes    | catwalk (2,4 m) |
//         |  hauts      |  la fen.  |             |    bloc est    |
//  z   2  |  (toit)  -- tunnels bas (toit) ------> |  MID           |
//  z   8  |                           |  enseigne  |            long|
//         |                bloc ouest |  caisse    |    bloc est   A|
//  z  32  |                           |            +-----------+long|
//  z  40  +----------------+----------+            | outside   doors|
//         |          T SPAWN (arcade)       maison T                 |
//  z  60  +---------------------------------------------------------+
//
// Tout est fait de boîtes alignées sur les axes, CONSTRUITES ici par des
// fonctions (mur percé d'une porte ou d'une fenêtre, escalier, caisse,
// tonneau, colonne...). Les escaliers se montent en marchant
// (STEP_HEIGHT), les caisses d'un mètre en sautant, les plateaux à
// 1,60 m par leurs escaliers. Un second tableau, MAP_DECOR, ne sert qu'au
// client : battants de portes, encadrements, auvents, tapis, créneaux.
//
// Les voies font au moins 4 m de large : les bots suivent des points de
// passage (NAV_NODES) et doivent pouvoir marcher droit de l'un à l'autre
// (le test de carte le fait marcher pour de vrai, escaliers compris).
Box MAP_BOXES[MAP_MAX_BOXES];
int MAP_BOX_COUNT = 0;
Box MAP_DECOR[MAP_MAX_BOXES];
int MAP_DECOR_COUNT = 0;

#define H_WALL  6.0f     // hauteur des grands blocs et de l'enceinte : infranchissable
#define H_ROOF0 3.2f     // dessous des toits de tunnel : un joueur (1,80 m) passe
#define H_ROOF1 4.0f

static float Min2(float a, float b) { return a < b ? a : b; }
static float Max2(float a, float b) { return a > b ? a : b; }

// Une boîte entre deux coins quelconques (l'ordre des coins est libre)
static void AddBox(Box *arr, int *n, float x0, float y0, float z0,
                   float x1, float y1, float z1, int kind)
{
    if (*n >= MAP_MAX_BOXES) return;
    // Un mur dont la porte touche le bout n'a pas de morceau de ce côté
    if (fabsf(x1 - x0) < 1e-4f || fabsf(y1 - y0) < 1e-4f || fabsf(z1 - z0) < 1e-4f) return;
    Box *b = &arr[(*n)++];
    b->min = (V3){Min2(x0, x1), Min2(y0, y1), Min2(z0, z1)};
    b->max = (V3){Max2(x0, x1), Max2(y0, y1), Max2(z0, z1)};
    b->kind = kind;
}
static void Solid(float x0, float y0, float z0, float x1, float y1, float z1, int kind)
{
    AddBox(MAP_BOXES, &MAP_BOX_COUNT, x0, y0, z0, x1, y1, z1, kind);
}
static void Decor(float x0, float y0, float z0, float x1, float y1, float z1, int kind)
{
    AddBox(MAP_DECOR, &MAP_DECOR_COUNT, x0, y0, z0, x1, y1, z1, kind);
}

// Mur plein, du sol à h
static void Wall(float x0, float z0, float x1, float z1, float h)
{
    Solid(x0, 0, z0, x1, h, z1, K_WALL);
}
// Mur le long de X (de x0 à x1, épaisseur z0..z1) percé d'une porte entre
// gx0 et gx1, avec son linteau à partir de lintel. Battants décoratifs.
static void DoorWallX(float x0, float x1, float z0, float z1, float h,
                      float gx0, float gx1, float lintel)
{
    Wall(x0, z0, gx0, z1, h);
    Wall(gx1, z0, x1, z1, h);
    Solid(gx0, lintel, z0, gx1, h, z1, K_WOOD);
    float t = (z0 + z1) / 2;
    Decor(gx0, 0, t - 0.04f, gx0 + 0.1f, lintel, t + 0.04f, K_WOOD);   // montants
    Decor(gx1 - 0.1f, 0, t - 0.04f, gx1, lintel, t + 0.04f, K_WOOD);
    float w = (gx1 - gx0) / 2 - 0.1f;                                   // battants ouverts
    Decor(gx0 + 0.1f, 0.05f, z1, gx0 + 0.18f, lintel - 0.1f, z1 + w, K_WOOD);
    Decor(gx1 - 0.18f, 0.05f, z1, gx1 - 0.1f, lintel - 0.1f, z1 + w, K_WOOD);
}
// Même chose le long de Z
static void DoorWallZ(float z0, float z1, float x0, float x1, float h,
                      float gz0, float gz1, float lintel)
{
    Wall(x0, z0, x1, gz0, h);
    Wall(x0, gz1, x1, z1, h);
    Solid(x0, lintel, gz0, x1, h, gz1, K_WOOD);
    float t = (x0 + x1) / 2;
    Decor(t - 0.04f, 0, gz0, t + 0.04f, lintel, gz0 + 0.1f, K_WOOD);
    Decor(t - 0.04f, 0, gz1 - 0.1f, t + 0.04f, lintel, gz1, K_WOOD);
    float w = (gz1 - gz0) / 2 - 0.1f;
    Decor(x1, 0.05f, gz0 + 0.1f, x1 + w, lintel - 0.1f, gz0 + 0.18f, K_WOOD);
    Decor(x1, 0.05f, gz1 - 0.18f, x1 + w, lintel - 0.1f, gz1 - 0.1f, K_WOOD);
}
// Mur le long de Z percé d'une fenêtre (entre gz0 et gz1, de sill à top) :
// on tire à travers, on ne passe pas. Encadrement décoratif.
static void WindowWallZ(float z0, float z1, float x0, float x1, float h,
                        float gz0, float gz1, float sill, float top)
{
    Wall(x0, z0, x1, gz0, h);
    Wall(x0, gz1, x1, z1, h);
    Solid(x0, 0, gz0, x1, sill, gz1, K_WALL);
    Solid(x0, top, gz0, x1, h, gz1, K_WALL);
    Decor(x0 - 0.05f, sill - 0.08f, gz0 - 0.08f, x1 + 0.05f, sill, gz1 + 0.08f, K_WOOD);
    Decor(x0 - 0.05f, top, gz0 - 0.08f, x1 + 0.05f, top + 0.08f, gz1 + 0.08f, K_WOOD);
    Decor(x0 - 0.05f, sill, gz0 - 0.08f, x1 + 0.05f, top, gz0, K_WOOD);
    Decor(x0 - 0.05f, sill, gz1, x1 + 0.05f, top, gz1 + 0.08f, K_WOOD);
}
static void WindowWallX(float x0, float x1, float z0, float z1, float h,
                        float gx0, float gx1, float sill, float top)
{
    Wall(x0, z0, gx0, z1, h);
    Wall(gx1, z0, x1, z1, h);
    Solid(gx0, 0, z0, gx1, sill, z1, K_WALL);
    Solid(gx0, top, z0, gx1, h, z1, K_WALL);
    Decor(gx0 - 0.08f, sill - 0.08f, z0 - 0.05f, gx1 + 0.08f, sill, z1 + 0.05f, K_WOOD);
    Decor(gx0 - 0.08f, top, z0 - 0.05f, gx1 + 0.08f, top + 0.08f, z1 + 0.05f, K_WOOD);
    Decor(gx0 - 0.08f, sill, z0 - 0.05f, gx0, top, z1 + 0.05f, K_WOOD);
    Decor(gx1, sill, z0 - 0.05f, gx1 + 0.08f, top, z1 + 0.05f, K_WOOD);
}
// Escalier le long de X : part de x_start, monte dans le sens dir (+1 ou
// -1), `steps` marches de `rise` de haut et `run` de profondeur, entre z0 et z1
static void StairsX(float x_start, int dir, float z0, float z1, int steps, float rise, float run)
{
    for (int i = 0; i < steps; i++) {
        float xa = x_start + dir * run * i, xb = x_start + dir * run * (i + 1);
        Solid(xa, 0, z0, xb, rise * (i + 1), z1, K_STEP);
    }
}
static void StairsZ(float z_start, int dir, float x0, float x1, int steps, float rise, float run)
{
    for (int i = 0; i < steps; i++) {
        float za = z_start + dir * run * i, zb = z_start + dir * run * (i + 1);
        Solid(x0, 0, za, x1, rise * (i + 1), zb, K_STEP);
    }
}
static void Crate(float cx, float cz, float size, float h)
{
    Solid(cx - size / 2, 0, cz - size / 2, cx + size / 2, h, cz + size / 2, K_CRATE);
}
// Deux caisses empilées, la seconde plus petite et décalée
static void CrateStack(float cx, float cz, float size, float h)
{
    Crate(cx, cz, size, h);
    float s2 = size * 0.65f;
    Solid(cx - s2 / 2 + 0.1f, h, cz - s2 / 2 - 0.1f, cx + s2 / 2 + 0.1f, h + s2 * 0.9f, cz + s2 / 2 - 0.1f, K_CRATE);
}
static void Barrel(float cx, float cz)
{
    Solid(cx - 0.4f, 0, cz - 0.4f, cx + 0.4f, 1.1f, cz + 0.4f, K_BARREL);
}
static void Barrels(float cx, float cz)   // un groupe de trois
{
    Barrel(cx, cz);
    Barrel(cx + 0.85f, cz + 0.3f);
    Barrel(cx + 0.3f, cz - 0.85f);
}
static void Sandbags(float x0, float z0, float x1, float z1)
{
    Solid(x0, 0, z0, x1, 1.0f, z1, K_SANDBAG);
}
static void Column(float cx, float cz, float r, float h)
{
    Solid(cx - r, 0, cz - r, cx + r, h, cz + r, K_STONE);
}
// Arcade le long de X : colonnes tous les `pitch` m, et une dalle de toit
// (qui arrête les balles) entre z0 et z1
static void ArcadeX(float x0, float x1, float z_col, float z0, float z1, float pitch, float y_roof)
{
    for (float x = x0; x <= x1 + 0.01f; x += pitch) Column(x, z_col, 0.45f, y_roof);
    Solid(x0 - 1.5f, y_roof, z0, x1 + 1.5f, y_roof + 0.5f, z1, K_ROOF);
    for (float x = x0; x <= x1 + 0.01f; x += pitch)            // chapiteaux
        Decor(x - 0.6f, y_roof - 0.3f, z_col - 0.6f, x + 0.6f, y_roof, z_col + 0.6f, K_STONE);
}
static void Awning(float x0, float z0, float x1, float z1, float y)
{
    Decor(x0, y, z0, x1, y + 0.12f, z1, K_AWNING);
}
static void Rug(float x0, float z0, float x1, float z1)
{
    Decor(x0, 0.01f, z0, x1, 0.03f, z1, K_CLOTH);
}

void MapInit(void)
{
    MAP_BOX_COUNT = 0;
    MAP_DECOR_COUNT = 0;
    const float A = ARENA_HALF;

    // ===== Enceinte, avec ses créneaux =====
    Wall(-A - 1, -A - 1,  A + 1, -A, H_WALL);     // nord
    Wall(-A - 1,  A,      A + 1,  A + 1, H_WALL); // sud
    Wall(-A - 1, -A,     -A,      A, H_WALL);     // ouest
    Wall( A,     -A,      A + 1,  A, H_WALL);     // est
    for (float c = -A; c < A; c += 4.0f) {
        Decor(c, H_WALL, -A - 1, c + 2, H_WALL + 0.6f, -A, K_WALL);
        Decor(c, H_WALL,  A,     c + 2, H_WALL + 0.6f,  A + 1, K_WALL);
        Decor(-A - 1, H_WALL, c, -A, H_WALL + 0.6f, c + 2, K_WALL);
        Decor( A,     H_WALL, c,  A + 1, H_WALL + 0.6f, c + 2, K_WALL);
    }

    // ===== Bloc ouest (entre les tunnels et mid) =====
    Wall(-46, -24, -18, 2, H_WALL);                 // la masse
    // La salle de la fenêtre, à l'angle nord-est du bloc : on y entre par
    // le connecteur (porte au nord) et on domine mid par une fenêtre à l'est
    DoorWallX(-18, -10, -24, -23, H_WALL, -15, -13, 2.4f);
    WindowWallZ(-24, -16, -11, -10, H_WALL, -22, -18, 1.0f, 2.0f);
    Wall(-18, -17, -10, -16, H_WALL);
    Wall(-18, -16, -10, 2, H_WALL);                 // le reste, sous la salle
    Crate(-16, -19, 1.0f, 1.0f);                    // une caisse pour se hisser à la fenêtre
    Awning(-15.5f, -24.9f, -12.5f, -24.0f, 2.5f);
    // Tunnels bas : une percée couverte à travers le bloc, vers mid
    Solid(-46, H_ROOF0, 2, -10, H_ROOF1, 8, K_ROOF);
    Sandbags(-32, 7.2f, -28, 8);
    for (float x = -42; x < -12; x += 8) Decor(x, 3.05f, 2, x + 0.3f, 3.2f, 8, K_WOOD);   // poutres
    Wall(-46, 8, -10, 30, H_WALL);
    Wall(-46, 30, -10, 40, 4.0f);                   // plus bas vers le spawn T

    // ===== Tunnels hauts (le long de l'enceinte ouest), couverts =====
    Solid(-A, H_ROOF0, -20, -46, H_ROOF1, 30, K_ROOF);
    for (float z = -16; z <= 28; z += 11) Column(-47, z, 0.5f, H_ROOF0);   // hors de la percée des tunnels bas
    for (float z = -18; z < 30; z += 6) Decor(-A, 3.05f, z, -46, 3.2f, z + 0.3f, K_WOOD);
    Sandbags(-57, 12, -54, 13);
    Barrels(-56, -10);
    Crate(-49, 23, 2.0f, 1.2f);
    Rug(-56, 36, -50, 40);

    // ===== Bloc nord-ouest (entre le connecteur et le passage CT -> B) =====
    Wall(-34, -42, -10, -30, H_WALL);
    // B doors : la porte du spawn CT vers le site B
    DoorWallZ(-58, -42, -36, -34, H_WALL, -46, -42, 2.6f);
    // Connecteur : couverture et l'arche d'entrée du site B
    Crate(-27, -24.8f, 2.0f, 1.0f);
    Sandbags(-20, -30, -17, -29);
    Column(-35, -29.5f, 0.4f, 4.0f);
    Column(-35, -24.5f, 0.4f, 4.0f);
    Decor(-35.4f, 4.0f, -29.9f, -34.6f, 4.4f, -24.1f, K_WOOD);

    // ===== Site B (nord-ouest) =====
    Solid(-56, 0, -56, -46, 1.6f, -50, K_STEP);     // le plateau
    StairsX(-42, -1, -56, -50, 4, 0.4f, 1.0f);      // ses marches, côté est
    // Le réduit : une petite pièce contre l'enceinte, porte à l'est
    Wall(-58, -46, -50, -45.5f, 3.5f);
    Wall(-58, -38.5f, -50, -38, 3.5f);
    DoorWallZ(-46, -38, -50.5f, -50, 3.5f, -43, -41, 2.3f);
    Solid(-58, 3.5f, -46, -50, 3.8f, -38, K_ROOF);
    Crate(-56, -41, 1.0f, 1.0f);
    Awning(-50, -43.5f, -48.8f, -40.5f, 2.4f);
    CrateStack(-40, -30, 2.2f, 2.0f);
    Barrels(-55, -30);
    Crate(-56, -32.5f, 1.5f, 1.5f);
    Crate(-49, -38, 2.0f, 2.0f);
    Sandbags(-44, -48, -41, -47);
    Crate(-37.5f, -50, 2.0f, 2.0f);
    Rug(-45, -45, -40, -41);

    // ===== Spawn CT (nord, centre) : une arcade le long de l'enceinte =====
    ArcadeX(-28, 14, -57, -A, -55, 6.0f, 4.0f);
    CrateStack(-14.5f, -45.5f, 3.0f, 1.2f);
    CrateStack(14.5f, -45.5f, 3.0f, 1.2f);
    Sandbags(4, -44.5f, 8, -43.5f);
    Sandbags(-8, -44.5f, -4, -43.5f);
    Wall(22, -48, 23, -44, 1.2f);                   // muret sur le passage CT -> A
    Barrels(31, -55.5f);
    Barrels(17, -55.5f);
    Rug(-4, -53, 4, -48);

    // ===== Mid =====
    // Top mid : couvertures basses de part et d'autre de l'axe
    Sandbags(-9, -40, -6, -37);
    Sandbags(6, -40, 9, -37);
    Crate(13.5f, -32.5f, 3.0f, 1.5f);
    // Les doubles portes : un mur percé d'un passage de 5 m, battants ouverts
    DoorWallX(-10, 10, -14, -12, 4.0f, -2.5f, 2.5f, 2.6f);
    // L'enseigne au poussin et la caisse « xbox »
    Solid(-1.5f, 0, -1.5f, 1.5f, 3, 1.5f, K_WALL);
    Solid(-1.5f, 0, 14.5f, 1.5f, 2, 17.5f, K_CRATE);
    Wall(-10, 10, -9, 16, 1.2f);                    // murets le long des blocs
    Wall(9, 10, 10, 16, 1.2f);
    Barrel(-8, 36);
    Barrels(7, 34);

    // ===== Catwalk : une passerelle à 2,40 m, du haut de mid au site A =====
    Wall(18, -42, 34, -24, H_WALL);                 // le bloc N1, au nord
    StairsX(10, 1, -24, -18, 6, 0.4f, 1.0f);        // on monte depuis top mid
    Solid(16, 0, -24, 38, 2.4f, -18, K_STEP);       // la passerelle
    StairsX(44, -1, -24, -18, 6, 0.4f, 1.0f);       // et on redescend sur le site A
    Solid(30, 2.4f, -19.5f, 32, 3.4f, -18, K_CRATE);
    Decor(34, 2.4f, -24.2f, 38, 3.4f, -24, K_METAL); // garde-corps côté site A
    Decor(16, 2.4f, -24.2f, 18, 3.4f, -24, K_METAL);

    // ===== Bloc est (entre mid et long A) =====
    Wall(10, -18, 34, 32, H_WALL);
    Wall(34, -18, 46, 0, 5.0f);                     // avec une alcôve sur long (z 0..6)
    Wall(34, 0, 42, 6, 5.0f);
    Wall(34, 6, 46, 36, 5.0f);
    Crate(44, 3, 1.5f, 1.5f);
    Crate(21, 34, 2.0f, 2.0f);                      // outside long
    Barrels(14, 36);

    // ===== Long doors : un passage couvert, une porte à chaque bout =====
    Wall(46, 32, 47, 38, 5.0f);
    Wall(57, 32, 58, 38, 5.0f);
    DoorWallX(46, 58, 32, 33, 5.0f, 50, 54, 2.6f);
    DoorWallX(46, 58, 37, 38, 5.0f, 50, 54, 2.6f);
    Solid(46, 4.0f, 32, 58, 5.0f, 38, K_ROOF);
    Rug(50.5f, 33.5f, 53.5f, 36.5f);

    // ===== Long A : le couloir de l'est =====
    Wall(46, -10, 47, 0, 1.2f);                     // muret côté bloc
    Crate(56, 19.5f, 2.0f, 2.0f);
    Sandbags(47, 12, 50, 13);
    Barrels(56, -20);                               // le coin de long

    // ===== Site A (nord-est) =====
    Solid(40, 0, -56, 52, 1.6f, -48, K_STEP);       // le plateau
    StairsX(36, 1, -56, -48, 4, 0.4f, 1.0f);        // ses marches, côté ouest
    StairsZ(-44, -1, 44, 50, 4, 0.4f, 1.0f);        // et côté sud
    CrateStack(40, -36, 2.0f, 2.0f);
    Solid(50, 0, -34, 54.5f, 2.2f, -32, K_METAL);   // un fourgon abandonné
    Solid(54.5f, 0, -34, 56, 1.6f, -32, K_METAL);   // sa cabine
    Decor(50.2f, 0.9f, -34.05f, 54.3f, 1.9f, -31.95f, K_CLOTH);   // sa bâche
    Crate(56, -27, 2.0f, 1.5f);
    Barrels(56.5f, -31.5f);
    Sandbags(42, -42, 45, -41);
    Crate(55, -38.5f, 2.0f, 1.2f);
    Column(35, -57, 0.5f, 4.5f);                    // l'arche CT -> A
    Column(35, -43, 0.5f, 4.5f);
    Decor(34.5f, 4.5f, -57.5f, 35.5f, 5.0f, -42.5f, K_WOOD);
    Column(47.5f, -25, 0.5f, 4.5f);                 // l'arche de long vers A
    Column(56.5f, -25, 0.5f, 4.5f);
    Decor(47, 4.5f, -25.5f, 57, 5.0f, -24.5f, K_WOOD);
    Rug(44, -40, 50, -34);

    // ===== Spawn T (sud) =====
    ArcadeX(-20, 16, 57, 55, A, 6.0f, 4.0f);
    // La maison T, à l'est du spawn : porte à l'ouest, fenêtre au nord
    DoorWallZ(48, 58, 30, 31, 4.0f, 52, 54, 2.3f);
    WindowWallX(31, 42, 48, 49, 4.0f, 34, 38, 1.0f, 2.0f);
    Wall(41, 49, 42, 58, 4.0f);
    Solid(30, 4.0f, 48, 42, 4.3f, 58, K_ROOF);
    Crate(38, 55, 1.5f, 1.0f);
    Awning(29.0f, 51.5f, 30.0f, 54.5f, 2.4f);
    Barrels(-30, 55);
    Crate(23, 42, 2.0f, 1.2f);
    Sandbags(-10, 42, -6, 43);
    // Des étals de marché : quatre poteaux, une toile, un comptoir
    for (int k = 0; k < 2; k++) {
        Column(-36 + k * 9.5f, 42, 0.15f, 2.6f);
        Column(-36 + k * 9.5f, 46, 0.15f, 2.6f);
    }
    Awning(-36.5f, 41.5f, -26, 46.5f, 2.5f);
    Solid(-35, 0, 43, -27.5f, 1.0f, 44, K_WOOD);
    Crate(-33, 45.2f, 1.2f, 0.8f);
    Crate(-29, 45.2f, 1.2f, 1.0f);
    // La remise, contre l'enceinte ouest : porte à l'est
    Wall(-58, 50, -52.5f, 50.5f, 3.5f);
    DoorWallZ(50, 58, -52.5f, -52, 3.5f, 53, 55, 2.3f);
    Solid(-58, 3.5f, 50, -52, 3.8f, 58, K_ROOF);
    Barrels(-56, 56);
    Barrels(-50, 46);
    Rug(-5, 48, 5, 54);
}

// Spawns : CT (rouges) au nord, T (bleus) au sud, alignés au centre de
// leur camp, face aux trois voies.
const V3 TEAM_SPAWNS[TEAM_COUNT][SPAWNS_PER_TEAM] = {
    // Rouge / CT : au nord (z négatif)
    {{-12, 0, -52}, {-6, 0, -50}, {0, 0, -52}, {6, 0, -50}, {12, 0, -52}},
    // Bleu / T : au sud (z positif)
    {{-12, 0, 52}, {-6, 0, 50}, {0, 0, 52}, {6, 0, 50}, {12, 0, 52}},
};

// Points de passage des bots. Chaque nœud est au milieu d'un couloir ou
// d'une zone ouverte ; d'un nœud à un nœud lié, on arrive en marchant
// droit (escaliers compris : le test de carte le vérifie).
const NavNode NAV_NODES[] = {
    /* 0  CT spawn        */ {{  0, 0, -50}, {1, 11, 17, -1}},
    /* 1  top mid         */ {{  0, 0, -30}, {0, 2, 7, 25, -1}},
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
    /* 16 catwalk (haut)  */ {{ 22, 2.4f, -21}, {25, 23, -1}},
    /* 17 CT -> A         */ {{ 26, 0, -50}, {0, 18, -1}},
    /* 18 site A          */ {{ 46, 0, -38}, {23, 17, 20, -1}},
    /* 19 T spawn est     */ {{ 30, 0,  44}, {5, 21, -1}},
    /* 20 long A          */ {{ 52, 0,  -8}, {18, 22, -1}},
    /* 21 outside long    */ {{ 52, 0,  40}, {19, 22, -1}},
    /* 22 long doors      */ {{ 52, 0,  34}, {20, 21, -1}},
    /* 23 bas du catwalk  */ {{ 45, 0, -21}, {16, 18, -1}},
    /* 24 entrée B (est)  */ {{-38, 0, -27}, {7, 9, -1}},
    /* 25 pied de l'esc.  */ {{  8, 0, -21}, {1, 16, -1}},
};
const int NAV_NODE_COUNT = (int)(sizeof(NAV_NODES) / sizeof(NAV_NODES[0]));

// Les sites de bombe : la zone au sol où un T peut poser
const Box BOMB_SITES[BOMB_SITE_COUNT] = {
    {{ 34, 0, -58}, { 58, 4, -24}, K_WALL},   // A : nord-est
    {{-58, 0, -58}, {-34, 4, -24}, K_WALL},   // B : nord-ouest
};
const int BOMB_SITE_NODE[BOMB_SITE_COUNT] = { 18, 9 };

int BombSiteAt(V3 pos)
{
    for (int i = 0; i < BOMB_SITE_COUNT; i++) {
        const Box *b = &BOMB_SITES[i];
        if (pos.x >= b->min.x && pos.x <= b->max.x && pos.z >= b->min.z && pos.z <= b->max.z
            && pos.y >= b->min.y && pos.y <= b->max.y) return i;
    }
    return -1;
}

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

// Un pas horizontal sur UN axe. Si un obstacle bloque et qu'on est au
// sol, on essaie de MONTER dessus : c'est une marche si son dessus est à
// moins de STEP_HEIGHT au-dessus des pieds (escaliers, trottoirs, bord
// d'un plateau), un mur sinon. Rend vrai si le joueur a bougé.
static bool StepAxis(V3 *pos, float dx, float dz, bool on_ground)
{
    V3 t = *pos;
    t.x += dx;
    t.z += dz;
    if (!CollidesMap(t)) { *pos = t; return true; }
    if (!on_ground) return false;

    Box pb = PlayerBox(t);
    float top = -1.0f;
    for (int i = 0; i < MAP_BOX_COUNT; i++) {
        const Box *b = &MAP_BOXES[i];
        if (!(pb.min.x < b->max.x && pb.max.x > b->min.x &&
              pb.min.z < b->max.z && pb.max.z > b->min.z)) continue;
        if (b->min.y >= pb.max.y) continue;                 // au-dessus de la tête
        if (b->max.y <= pos->y) continue;                   // sous les pieds
        if (b->max.y > pos->y + STEP_HEIGHT) return false;  // trop haut : un mur
        if (b->max.y > top) top = b->max.y;
    }
    if (top < 0.0f) return false;
    t.y = top;                                              // posé sur la marche
    if (CollidesMap(t)) return false;                       // plafond trop bas
    *pos = t;
    return true;
}

// Déplace un joueur en résolvant les collisions avec la map.
// Principe : on essaie le déplacement axe par axe (x, puis z, puis y).
// Si la position d'arrivée traverse une boîte, on annule seulement cet
// axe-là : le joueur "glisse" le long du mur au lieu de s'arrêter net.
// Une marche basse se franchit en marchant (StepAxis).
void MovePlayer(V3 *pos, V3 vel_h, float *vel_y, float dt, bool *on_ground)
{
    // --- Horizontal : axe X puis axe Z, indépendamment ---
    StepAxis(pos, vel_h.x * dt, 0.0f, *on_ground);
    StepAxis(pos, 0.0f, vel_h.z * dt, *on_ground);

    // --- Vertical : gravité puis test de l'atterrissage ---
    *vel_y -= GRAVITY * dt;        // la vitesse verticale diminue en continu
    V3 try_pos = *pos;
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
    c.use    = (e->flags & INPUT_USE) != 0;
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
