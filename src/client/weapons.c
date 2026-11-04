// weapons.c — Modèles 3D des armes (voir weapons.h)
#include <math.h>

#include "raylib.h"
#include "raymath.h"

#include "../common/protocol.h"
#include "models.h"
#include "weapons.h"

#define P3(x, y, z) ((Vector3){ (x), (y), (z) })
#define NOROT P3(0, 0, 0)

// Palette
static const Color METAL   = {38, 38, 36, 255};     // acier sombre
static const Color METAL2  = {55, 54, 50, 255};     // acier plus clair (contraste)
static const Color POLYMER = {30, 30, 30, 255};     // plastique (poignée, crosse)
static const Color TAN     = {150, 132, 96, 255};   // polymère sable (HK416)
static const Color PLUM    = {78, 44, 66, 255};     // bakélite « prune » des chargeurs AK
static const Color OLIVE_PLY   = {52, 62, 46, 255};     // polymère vert-noir (FAMAS)
static const Color DARK    = {20, 20, 20, 255};
static const Color GLASS   = {120, 170, 210, 255};  // lentille
static const Color BRASS   = {200, 170, 90, 255};

//----------------------------------------------------------------------------------
// Pièces communes
//----------------------------------------------------------------------------------
// Boîtier : une boîte de z0 à z1, de largeur w et de hauteur h, centrée en y
static void Block(Matrix m, float z0, float z1, float w, float h, float y, Color c)
{
    Box3(m, P3(0, y, (z0 + z1) / 2), NOROT, P3(w, h, fabsf(z1 - z0)), c);
}
// Canon (cylindre sur l'axe, à la hauteur y)
static void Barrel(Matrix m, float z0, float z1, float r, float y, Color c)
{
    Seg(m, P3(0, y, z0), P3(0, y, z1), r, c);
}
// Poignée pistolet, un peu inclinée vers l'arrière
static void Grip(Matrix m, float z, float h, Color c)
{
    Box3(m, P3(0, -0.03f - h / 2, z + 0.012f), P3(-0.25f, 0, 0), P3(0.034f, h, 0.045f), c);
}
// Chargeur droit ou courbé (deux boîtes), descendu de `out` pendant le rechargement
static void Magazine(Matrix m, float z, float len, float curve, float out, Color c)
{
    float y0 = -0.045f - out * 0.25f;
    Box3(m, P3(0, y0 - len * 0.27f, z), NOROT, P3(0.03f, len * 0.55f, 0.06f), c);
    Box3(m, P3(0, y0 - len * 0.75f, z - curve), P3(curve * 3.0f, 0, 0),
         P3(0.031f, len * 0.5f, 0.06f), c);
}
// Hausse et guidon : deux montants + point rouge sur la ligne de visée
static void IronSights(Matrix m, float z_rear, float z_front, Color c)
{
    Box3(m, P3(0, WEAPON_SIGHT_Y - 0.02f, z_rear), NOROT, P3(0.012f, 0.03f, 0.012f), c);
    Box3(m, P3(0, WEAPON_SIGHT_Y - 0.02f, z_front), NOROT, P3(0.012f, 0.03f, 0.012f), c);
    Ball(m, P3(0, WEAPON_SIGHT_Y, z_rear), P3(0.004f, 0.004f, 0.004f), (Color){255, 40, 40, 255});
}
// Lunette : tube sur la ligne de visée, avec ses deux lentilles
static void Scope(Matrix m, float z0, float z1)
{
    Seg(m, P3(0, WEAPON_SIGHT_Y, z0), P3(0, WEAPON_SIGHT_Y, z1), 0.018f, DARK);
    Seg(m, P3(0, WEAPON_SIGHT_Y, z1 - 0.01f), P3(0, WEAPON_SIGHT_Y, z1), 0.021f, GLASS);
    Seg(m, P3(0, WEAPON_SIGHT_Y, z0), P3(0, WEAPON_SIGHT_Y, z0 + 0.01f), 0.019f, GLASS);
    // Montage sur le rail
    Box3(m, P3(0, 0.05f, z0 + 0.03f), NOROT, P3(0.02f, 0.03f, 0.02f), METAL);
    Box3(m, P3(0, 0.05f, z1 - 0.03f), NOROT, P3(0.02f, 0.03f, 0.02f), METAL);
}
// Crosse (fixe ou télescopique)
static void Stock(Matrix m, float z0, float z1, float h, Color c)
{
    Box3(m, P3(0, -0.015f, (z0 + z1) / 2), NOROT, P3(0.04f, h, z1 - z0), c);
}
// Flash de bouche : boule + étoile de 4 branches, à la taille demandée
static void MuzzleFlash(Matrix m, Vector3 at, float k)
{
    if (k <= 0.0f) return;
    Ball(m, at, P3(0.03f * k, 0.03f * k, 0.04f * k), (Color){255, 240, 170, 255});
    Box3(m, at, NOROT, P3(0.14f * k, 0.012f, 0.012f), (Color){255, 180, 60, 255});
    Box3(m, at, NOROT, P3(0.012f, 0.14f * k, 0.012f), (Color){255, 180, 60, 255});
    Box3(m, P3(at.x, at.y, at.z - 0.04f * k), NOROT, P3(0.012f, 0.012f, 0.1f * k), (Color){255, 220, 120, 255});
}

//----------------------------------------------------------------------------------
// Les armes
//----------------------------------------------------------------------------------
static void DrawFamas(Matrix m, const WeaponPose *p)
{
    // Bullpup : le chargeur est DERRIÈRE la poignée, dans la crosse ; la
    // grande poignée de transport surmonte tout le boîtier.
    Block(m, -0.30f, 0.30f, 0.05f, 0.07f, -0.005f, OLIVE_PLY);          // boîtier + crosse d'un bloc
    Block(m, 0.18f, 0.32f, 0.052f, 0.09f, -0.02f, Shade(OLIVE_PLY, 0.9f)); // talon de crosse
    Grip(m, -0.06f, 0.1f, POLYMER);
    Box3(m, P3(0, -0.075f, -0.14f), NOROT, P3(0.02f, 0.025f, 0.08f), POLYMER);   // pontet long
    Magazine(m, 0.12f, 0.18f, 0.015f, p->mag_out, METAL2);
    // Poignée de transport : deux montants et une poutre, hausse au bout
    Box3(m, P3(0, 0.045f, -0.2f), NOROT, P3(0.02f, 0.03f, 0.02f), OLIVE_PLY);
    Box3(m, P3(0, 0.045f, 0.1f), NOROT, P3(0.02f, 0.03f, 0.02f), OLIVE_PLY);
    Box3(m, P3(0, 0.07f, -0.05f), NOROT, P3(0.03f, 0.028f, 0.34f), OLIVE_PLY);
    IronSights(m, -0.04f, -0.2f, DARK);
    // Garde-main court et canon avec son cache-flamme
    Block(m, -0.42f, -0.3f, 0.045f, 0.05f, -0.01f, Shade(OLIVE_PLY, 0.85f));
    Barrel(m, -0.42f, -0.6f, 0.011f, 0.0f, METAL);
    Barrel(m, -0.56f, -0.63f, 0.015f, 0.0f, DARK);
    // Bipied replié sous le garde-main
    for (int k = -1; k <= 1; k += 2)
        Seg(m, P3(0.018f * k, -0.03f, -0.36f), P3(0.02f * k, -0.045f, -0.5f), 0.004f, METAL2);
    MuzzleFlash(m, P3(0, 0, -0.67f), p->muzzle);
}

// Plateforme AR : HK416 (garde-main à rails, crosse sable) et M4A1
// (garde-main rond, guidon triangulaire, crosse noire)
static void DrawAR(Matrix m, const WeaponPose *p, bool hk)
{
    Color furn = hk ? TAN : POLYMER;
    Block(m, -0.10f, 0.14f, 0.05f, 0.06f, 0.0f, METAL);                 // boîtier haut
    Block(m, -0.08f, 0.12f, 0.045f, 0.045f, -0.048f, METAL2);          // boîtier bas
    Box3(m, P3(0, 0.034f, 0.0f), NOROT, P3(0.024f, 0.012f, 0.24f), METAL);  // rail supérieur
    Grip(m, 0.09f, 0.1f, furn);
    Box3(m, P3(0, -0.075f, 0.02f), NOROT, P3(0.02f, 0.02f, 0.07f), METAL2);  // pontet
    Magazine(m, -0.03f, 0.16f, 0.012f, p->mag_out, hk ? METAL2 : POLYMER);
    // Garde-main
    if (hk) {
        Block(m, -0.42f, -0.10f, 0.055f, 0.055f, 0.0f, furn);          // quadrirail carré
        for (int i = 0; i < 6; i++)   // les crans du rail
            Box3(m, P3(0, 0.03f, -0.15f - i * 0.045f), NOROT, P3(0.03f, 0.008f, 0.02f), METAL);
    } else {
        Seg(m, P3(0, 0, -0.1f), P3(0, 0, -0.36f), 0.027f, furn);       // garde-main rond
        Box3(m, P3(0, 0.055f, -0.38f), NOROT, P3(0.012f, 0.05f, 0.03f), METAL);   // guidon triangulaire
        Box3(m, P3(0, 0.03f, -0.39f), NOROT, P3(0.04f, 0.02f, 0.03f), METAL);
    }
    // Culasse : le levier d'armement recule au réarmement
    Box3(m, P3(0.03f, 0.0f, 0.04f + p->bolt * 0.05f), NOROT, P3(0.015f, 0.02f, 0.03f), METAL2);
    // Tube de crosse et crosse télescopique
    Seg(m, P3(0, 0.0f, 0.14f), P3(0, 0.0f, 0.3f), 0.016f, METAL);
    Stock(m, 0.22f, 0.36f, 0.07f, furn);
    Box3(m, P3(0, -0.03f, 0.37f), NOROT, P3(0.045f, 0.12f, 0.03f), furn);     // plaque de couche
    // Canon et cache-flamme
    Barrel(m, hk ? -0.42f : -0.36f, -0.6f, 0.011f, 0.0f, METAL);
    Barrel(m, -0.56f, -0.63f, 0.015f, 0.0f, DARK);
    IronSights(m, 0.02f, hk ? -0.4f : -0.38f, DARK);
    MuzzleFlash(m, P3(0, 0, -0.67f), p->muzzle);
}

static void DrawAK(Matrix m, const WeaponPose *p)
{
    Block(m, -0.12f, 0.16f, 0.048f, 0.06f, 0.0f, METAL);              // boîtier
    Box3(m, P3(0, 0.035f, -0.2f), NOROT, P3(0.02f, 0.018f, 0.26f), METAL2);   // tube à gaz
    Seg(m, P3(0, 0.0f, -0.12f), P3(0, 0.0f, -0.34f), 0.026f, POLYMER);        // garde-main
    Box3(m, P3(0, -0.02f, -0.23f), NOROT, P3(0.05f, 0.03f, 0.2f), POLYMER);
    Grip(m, 0.1f, 0.095f, POLYMER);
    Box3(m, P3(0, -0.07f, 0.03f), NOROT, P3(0.018f, 0.02f, 0.08f), METAL2);   // pontet
    Magazine(m, -0.02f, 0.2f, 0.04f, p->mag_out, PLUM);                        // chargeur courbé « prune »
    // Levier d'armement à droite, recule au réarmement
    Box3(m, P3(0.032f, 0.01f, 0.0f + p->bolt * 0.06f), NOROT, P3(0.02f, 0.015f, 0.03f), METAL2);
    // Crosse repliable latérale (dépliée)
    Stock(m, 0.16f, 0.42f, 0.055f, POLYMER);
    Box3(m, P3(0, -0.02f, 0.43f), NOROT, P3(0.04f, 0.1f, 0.025f), POLYMER);
    // Canon, frein de bouche cylindrique caractéristique
    Barrel(m, -0.34f, -0.55f, 0.011f, 0.0f, METAL);
    Barrel(m, -0.52f, -0.62f, 0.017f, 0.0f, METAL2);
    IronSights(m, -0.1f, -0.5f, DARK);
    MuzzleFlash(m, P3(0, 0, -0.66f), p->muzzle);
}

static void DrawMP5(Matrix m, const WeaponPose *p)
{
    Seg(m, P3(0, 0.005f, -0.12f), P3(0, 0.005f, 0.1f), 0.025f, METAL);        // boîtier tubulaire
    Block(m, -0.06f, 0.1f, 0.04f, 0.045f, -0.03f, METAL2);                      // mécanisme
    Grip(m, 0.07f, 0.09f, POLYMER);
    Box3(m, P3(0, -0.065f, -0.0f), NOROT, P3(0.018f, 0.02f, 0.06f), POLYMER);   // pontet
    Magazine(m, -0.07f, 0.17f, 0.035f, p->mag_out, METAL2);                      // chargeur courbé
    Seg(m, P3(0, 0.0f, -0.12f), P3(0, 0.0f, -0.26f), 0.028f, POLYMER);          // garde-main rond
    Box3(m, P3(0.03f, 0.02f, -0.05f + p->bolt * 0.05f), NOROT, P3(0.015f, 0.012f, 0.025f), METAL2);
    // Crosse rétractable : deux tiges et une plaque
    for (int k = -1; k <= 1; k += 2)
        Seg(m, P3(0.015f * k, 0.0f, 0.1f), P3(0.015f * k, 0.0f, 0.26f), 0.005f, METAL2);
    Box3(m, P3(0, -0.01f, 0.27f), NOROT, P3(0.04f, 0.08f, 0.02f), POLYMER);
    // Canon court, hausse tambour
    Barrel(m, -0.26f, -0.36f, 0.009f, 0.0f, METAL);
    Seg(m, P3(0, WEAPON_SIGHT_Y - 0.02f, -0.02f), P3(0, WEAPON_SIGHT_Y - 0.02f, 0.02f), 0.014f, DARK);
    IronSights(m, 0.0f, -0.24f, DARK);
    MuzzleFlash(m, P3(0, 0, -0.4f), p->muzzle);
}

static void DrawFRF2(Matrix m, const WeaponPose *p)
{
    // Fût et crosse à trou de pouce d'une pièce, long canon gainé, bipied
    Block(m, -0.36f, 0.12f, 0.05f, 0.065f, -0.02f, POLYMER);              // fût
    Block(m, 0.12f, 0.46f, 0.045f, 0.075f, -0.025f, POLYMER);             // crosse
    Box3(m, P3(0, -0.07f, 0.47f), NOROT, P3(0.05f, 0.12f, 0.03f), DARK);    // plaque de couche
    Box3(m, P3(0, -0.09f, 0.3f), NOROT, P3(0.04f, 0.06f, 0.14f), POLYMER);  // bas de crosse
    Grip(m, 0.1f, 0.08f, POLYMER);
    Block(m, -0.1f, 0.1f, 0.044f, 0.05f, 0.015f, METAL);                   // boîtier de culasse
    Magazine(m, -0.04f, 0.1f, 0.0f, p->mag_out, METAL2);                    // chargeur court (10 coups)
    // Levier de culasse : il se relève et recule au réarmement
    {
        float b = p->bolt;
        Vector3 base = P3(0.02f, 0.03f, 0.04f + b * 0.06f);
        Vector3 tip  = P3(0.06f, 0.03f - 0.05f * (1.0f - b) + 0.02f * b, 0.05f + b * 0.06f);
        Seg(m, base, tip, 0.006f, METAL2);
        Ball(m, tip, P3(0.012f, 0.012f, 0.012f), METAL2);
    }
    Barrel(m, -0.36f, -0.7f, 0.015f, 0.01f, METAL2);                        // manchon thermique
    Barrel(m, -0.7f, -0.86f, 0.011f, 0.01f, METAL);                         // canon
    Barrel(m, -0.84f, -0.92f, 0.016f, 0.01f, DARK);                         // frein de bouche
    Scope(m, -0.16f, 0.08f);
    for (int k = -1; k <= 1; k += 2)   // bipied déplié
        Seg(m, P3(0.02f * k, -0.03f, -0.5f), P3(0.09f * k, -0.2f, -0.52f), 0.005f, METAL2);
    MuzzleFlash(m, P3(0, 0.01f, -0.96f), p->muzzle);
}

static void DrawBenelli(Matrix m, const WeaponPose *p)
{
    Block(m, -0.12f, 0.12f, 0.045f, 0.06f, 0.0f, METAL);                   // boîtier
    Grip(m, 0.09f, 0.095f, POLYMER);
    Box3(m, P3(0, -0.07f, 0.02f), NOROT, P3(0.018f, 0.02f, 0.06f), METAL2);  // pontet
    Stock(m, 0.12f, 0.42f, 0.06f, POLYMER);
    Box3(m, P3(0, -0.03f, 0.43f), NOROT, P3(0.045f, 0.11f, 0.03f), DARK);
    Barrel(m, -0.12f, -0.62f, 0.012f, 0.015f, METAL);                        // canon
    Barrel(m, -0.12f, -0.56f, 0.013f, -0.02f, METAL2);                       // tube-magasin dessous
    // Pompe : elle recule au réarmement
    Seg(m, P3(0, -0.005f, -0.3f + p->bolt * 0.08f), P3(0, -0.005f, -0.44f + p->bolt * 0.08f), 0.027f, POLYMER);
    for (int i = 0; i < 4; i++)   // les rainures de la pompe
        Box3(m, P3(0, -0.005f, -0.32f - i * 0.03f + p->bolt * 0.08f), NOROT, P3(0.058f, 0.058f, 0.006f), DARK);
    // Bande de visée et guidon (sur la ligne de visée)
    Box3(m, P3(0, 0.04f, -0.3f), NOROT, P3(0.012f, 0.008f, 0.5f), METAL2);
    IronSights(m, -0.1f, -0.58f, DARK);
    // Quatre cartouches sur le côté de la crosse
    for (int i = 0; i < 4; i++)
        Seg(m, P3(0.03f, 0.0f, 0.16f + i * 0.045f), P3(0.03f, 0.0f, 0.19f + i * 0.045f), 0.008f,
            i % 2 ? (Color){190, 30, 30, 255} : BRASS);
    MuzzleFlash(m, P3(0, 0.015f, -0.68f), p->muzzle * 1.4f);
}

// Pistolets : le Glock (glissière carrée, polymère) et le PAMAS (glissière
// ouverte sur le dessus, canon apparent, acier)
static void DrawPistol(Matrix m, const WeaponPose *p, bool glock)
{
    float z0 = -0.1f, z1 = 0.06f;
    if (glock) {
        Block(m, z0, z1, 0.03f, 0.03f, 0.0f, POLYMER);                      // glissière
        Block(m, z0 + 0.01f, z1 - 0.02f, 0.031f, 0.012f, 0.012f, DARK);
    } else {
        Block(m, z0 + 0.03f, z1, 0.03f, 0.03f, 0.0f, METAL2);               // glissière arrière
        Barrel(m, z0 + 0.03f, z0 - 0.01f, 0.009f, 0.004f, METAL);          // canon apparent
        Block(m, z0, z0 + 0.04f, 0.03f, 0.02f, -0.01f, METAL2);            // avant du bâti
    }
    // La glissière recule après chaque coup
    Box3(m, P3(0.016f, 0.0f, 0.02f + p->bolt * 0.03f), NOROT, P3(0.004f, 0.02f, 0.03f), DARK);
    Block(m, z0 + 0.02f, z1 - 0.01f, 0.028f, 0.025f, -0.025f, glock ? POLYMER : METAL);   // carcasse
    Box3(m, P3(0, -0.07f, 0.035f), P3(-0.3f, 0, 0), P3(0.026f, 0.085f, 0.035f), glock ? POLYMER : (Color){60, 50, 45, 255}); // poignée
    Box3(m, P3(0, -0.05f, -0.02f), NOROT, P3(0.012f, 0.016f, 0.05f), DARK);   // pontet
    Box3(m, P3(0, -0.045f, -0.005f), NOROT, P3(0.004f, 0.02f, 0.008f), METAL2); // détente
    // Chargeur : sort par le bas de la poignée au rechargement
    Box3(m, P3(0, -0.11f - p->mag_out * 0.15f, 0.04f), P3(-0.3f, 0, 0), P3(0.02f, 0.02f, 0.03f), METAL2);
    // Hausse et guidon sur la glissière, remontés sur la ligne de visée
    Box3(m, P3(0, 0.022f, 0.05f), NOROT, P3(0.02f, 0.012f, 0.008f), DARK);
    Box3(m, P3(0, 0.022f, -0.09f), NOROT, P3(0.006f, 0.012f, 0.008f), DARK);
    Ball(m, P3(0, WEAPON_SIGHT_Y, 0.05f), P3(0.0035f, 0.0035f, 0.0035f), (Color){255, 40, 40, 255});
    Box3(m, P3(0, (WEAPON_SIGHT_Y + 0.025f) / 2, 0.05f), NOROT, P3(0.006f, WEAPON_SIGHT_Y - 0.025f, 0.006f), DARK);
    MuzzleFlash(m, P3(0, 0.004f, -0.14f), p->muzzle * 0.8f);
}

void DrawWeaponModel(Matrix parent, int weapon, const WeaponPose *pose)
{
    static const WeaponPose rest = {0};
    if (!pose) pose = &rest;
    switch (weapon) {
    case WPN_FAMAS:   DrawFamas(parent, pose); break;
    case WPN_HK416:   DrawAR(parent, pose, true); break;
    case WPN_M4A1:    DrawAR(parent, pose, false); break;
    case WPN_AK74:    DrawAK(parent, pose); break;
    case WPN_MP5:     DrawMP5(parent, pose); break;
    case WPN_FRF2:    DrawFRF2(parent, pose); break;
    case WPN_BENELLI: DrawBenelli(parent, pose); break;
    case WPN_GLOCK:   DrawPistol(parent, pose, true); break;
    case WPN_PAMAS:   DrawPistol(parent, pose, false); break;
    default: break;   // griffes : rien à dessiner
    }
}

Vector3 WeaponMuzzle(int weapon)
{
    switch (weapon) {
    case WPN_FAMAS:   return P3(0, 0, -0.65f);
    case WPN_HK416:
    case WPN_M4A1:    return P3(0, 0, -0.65f);
    case WPN_AK74:    return P3(0, 0, -0.64f);
    case WPN_MP5:     return P3(0, 0, -0.38f);
    case WPN_FRF2:    return P3(0, 0.01f, -0.94f);
    case WPN_BENELLI: return P3(0, 0.015f, -0.66f);
    case WPN_GLOCK:
    case WPN_PAMAS:   return P3(0, 0.004f, -0.13f);
    default:          return P3(0, 0, -0.3f);
    }
}

void WeaponGrips(int weapon, Vector3 *right, Vector3 *left)
{
    switch (weapon) {
    case WPN_FAMAS:   *right = P3(0, -0.09f, -0.05f); *left = P3(-0.01f, -0.04f, -0.36f); break;
    case WPN_HK416:
    case WPN_M4A1:    *right = P3(0, -0.09f, 0.1f);   *left = P3(-0.01f, -0.04f, -0.3f); break;
    case WPN_AK74:    *right = P3(0, -0.09f, 0.11f);  *left = P3(-0.01f, -0.045f, -0.26f); break;
    case WPN_MP5:     *right = P3(0, -0.085f, 0.08f); *left = P3(-0.01f, -0.035f, -0.2f); break;
    case WPN_FRF2:    *right = P3(0, -0.08f, 0.11f);  *left = P3(-0.01f, -0.05f, -0.32f); break;
    case WPN_BENELLI: *right = P3(0, -0.09f, 0.1f);   *left = P3(-0.01f, -0.035f, -0.37f); break;
    case WPN_GLOCK:
    case WPN_PAMAS:   *right = P3(0, -0.07f, 0.035f); *left = P3(-0.03f, -0.07f, 0.02f); break;
    default:          *right = P3(0, -0.09f, 0.05f);  *left = P3(0, -0.04f, -0.3f); break;
    }
}

bool WeaponHasScope(int weapon) { return weapon == WPN_FRF2; }
