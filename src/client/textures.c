// textures.c — Génération des textures (voir textures.h)
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"

#include "models.h"
#include "textures.h"

#define TEX_SIZE 256

static Texture2D textures[MAT_COUNT];
static bool      ready = false;

//----------------------------------------------------------------------------------
// Bruit
//----------------------------------------------------------------------------------
// Hachage entier -> [0, 1) : un bruit « blanc » reproductible, sans table
static float Hash2(int x, int y, int seed)
{
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + (unsigned)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFF) / (float)0x1000000;
}

static float SmoothStep(float t) { return t * t * (3.0f - 2.0f * t); }

// Bruit de valeur : interpolation douce entre des valeurs aléatoires
// posées sur une grille de pas `cell` pixels ; périodique sur TEX_SIZE
// (la texture se répète sans couture).
static float ValueNoise(float x, float y, int cell, int seed)
{
    int n = TEX_SIZE / cell;
    float fx = x / cell, fy = y / cell;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float tx = SmoothStep(fx - x0), ty = SmoothStep(fy - y0);
    int xa = ((x0 % n) + n) % n, xb = (xa + 1) % n;
    int ya = ((y0 % n) + n) % n, yb = (ya + 1) % n;
    float a = Hash2(xa, ya, seed), b = Hash2(xb, ya, seed);
    float c = Hash2(xa, yb, seed), d = Hash2(xb, yb, seed);
    return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
}

// Somme d'octaves (fBm) : du relief à toutes les échelles, dans [0, 1]
static float Fbm(float x, float y, int cell, int octaves, int seed)
{
    float sum = 0, amp = 1, norm = 0;
    for (int o = 0; o < octaves && cell >= 2; o++) {
        sum += ValueNoise(x, y, cell, seed + o * 17) * amp;
        norm += amp;
        amp *= 0.5f;
        cell /= 2;
    }
    return sum / norm;
}

static unsigned char Clamp8(float v) { return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

typedef Color (*TexelFn)(int x, int y);

static Texture2D Generate(TexelFn fn)
{
    Image img = GenImageColor(TEX_SIZE, TEX_SIZE, WHITE);
    Color *px = (Color *)img.data;
    for (int y = 0; y < TEX_SIZE; y++)
        for (int x = 0; x < TEX_SIZE; x++)
            px[y * TEX_SIZE + x] = fn(x, y);
    Texture2D t = LoadTextureFromImage(img);
    UnloadImage(img);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
    return t;
}

//----------------------------------------------------------------------------------
// Les matières
//----------------------------------------------------------------------------------
// Sable : grain fin, ondulations larges, quelques cailloux sombres
static Color Sand(int x, int y)
{
    float n = Fbm((float)x, (float)y, 64, 5, 1);
    float g = Hash2(x, y, 99);
    float v = 150 + n * 60 + (g - 0.5f) * 18;
    if (Hash2(x / 3, y / 3, 7) > 0.985f) v -= 40;     // cailloux
    return (Color){Clamp8(v + 20), Clamp8(v - 2), Clamp8(v - 45), 255};
}

// Pisé : plâtre tacheté, lignes de briques estompées, fissures
static Color Wall(int x, int y)
{
    float n = Fbm((float)x, (float)y, 32, 4, 2);
    float v = 175 + n * 55;
    int row = y / 32;
    int bx = (x + (row % 2) * 32) % 64;
    if (y % 32 < 2 || bx < 2) v -= 18 * (0.4f + 0.6f * Fbm((float)x, (float)y, 16, 2, 3));   // joints
    // Fissures : lignes fines qui suivent un bruit
    float c = Fbm((float)x, (float)y, 48, 3, 4);
    if (fabsf(c - 0.5f) < 0.006f) v -= 50;
    return (Color){Clamp8(v + 22), Clamp8(v + 2), Clamp8(v - 40), 255};
}

// Bois : planches verticales de 32 px, veines ondulées, joints sombres
static Color Wood(int x, int y)
{
    int plank = x / 32;
    float grain = sinf((y * 0.09f) + Fbm((float)x, (float)y, 16, 3, 5 + plank) * 6.0f + plank * 1.7f);
    float v = 150 + grain * 18 + (Fbm((float)x, (float)y, 8, 2, 6) - 0.5f) * 24;
    if (x % 32 < 2) v -= 55;                                   // joint entre planches
    if (y % 128 < 2 && Hash2(plank, y / 128, 8) > 0.5f) v -= 35; // bout de planche
    return (Color){Clamp8(v + 25), Clamp8(v - 10), Clamp8(v - 55), 255};
}

// Pierre : blocs de 64 x 32 décalés, mortier, grain
static Color Stone(int x, int y)
{
    int row = y / 32;
    int bx = (x + (row % 2) * 32) % 64;
    float n = Fbm((float)x, (float)y, 16, 3, 9 + row);
    float v = 150 + n * 40 + (Hash2(x / 64 + row * 7, row, 10) - 0.5f) * 30;   // chaque bloc sa teinte
    if (y % 32 < 3 || bx < 3) v = 95 + n * 20;                                  // mortier
    return (Color){Clamp8(v), Clamp8(v - 4), Clamp8(v - 12), 255};
}

// Tôle : gris froid, rayures, taches de rouille
static Color Metal(int x, int y)
{
    float n = Fbm((float)x, (float)y, 32, 3, 11);
    float v = 125 + n * 30;
    if (Hash2(x, y / 7, 12) > 0.97f) v += 25;                                   // rayures
    float rust = Fbm((float)x, (float)y, 64, 4, 13);
    if (rust > 0.62f) {
        float k = (rust - 0.62f) / 0.2f;
        if (k > 1) k = 1;
        return (Color){Clamp8(v + 60 * k), Clamp8(v - 20 * k), Clamp8(v - 70 * k), 255};
    }
    return (Color){Clamp8(v), Clamp8(v + 2), Clamp8(v + 6), 255};
}

// Toile : tissage serré, légère usure
static Color Cloth(int x, int y)
{
    float weave = ((x / 2 + y / 2) % 2) ? 1.0f : 0.0f;
    float n = Fbm((float)x, (float)y, 32, 3, 14);
    float v = 165 + weave * 25 + (n - 0.5f) * 40;
    return (Color){Clamp8(v), Clamp8(v - 2), Clamp8(v - 6), 255};
}

// Tuiles : rangées de 32 px en quinconce, bord inférieur ombré
static Color Roof(int x, int y)
{
    int row = y / 32;
    int tx = (x + (row % 2) * 16) % 32;
    float n = Fbm((float)x, (float)y, 16, 3, 15 + row);
    float v = 150 + n * 35 + (Hash2(x / 32 + row * 3, row, 16) - 0.5f) * 20;
    int ty = y % 32;
    if (ty > 26) v -= (ty - 26) * 10;                 // ombre sous la tuile
    if (tx < 2) v -= 25;                              // joint
    return (Color){Clamp8(v + 10), Clamp8(v - 5), Clamp8(v - 25), 255};
}

void TexturesInit(void)
{
    if (ready) return;
    textures[MAT_SAND]  = Generate(Sand);
    textures[MAT_WALL]  = Generate(Wall);
    textures[MAT_WOOD]  = Generate(Wood);
    textures[MAT_STONE] = Generate(Stone);
    textures[MAT_METAL] = Generate(Metal);
    textures[MAT_CLOTH] = Generate(Cloth);
    textures[MAT_ROOF]  = Generate(Roof);
    BatchSetTexture(MAT_SAND,  textures[MAT_SAND],  4.0f);
    BatchSetTexture(MAT_WALL,  textures[MAT_WALL],  2.5f);
    BatchSetTexture(MAT_WOOD,  textures[MAT_WOOD],  1.2f);
    BatchSetTexture(MAT_STONE, textures[MAT_STONE], 1.5f);
    BatchSetTexture(MAT_METAL, textures[MAT_METAL], 1.0f);
    BatchSetTexture(MAT_CLOTH, textures[MAT_CLOTH], 0.6f);
    BatchSetTexture(MAT_ROOF,  textures[MAT_ROOF],  2.0f);
    ready = true;
}

void TexturesClose(void)
{
    if (!ready) return;
    for (int m = MAT_FLAT + 1; m < MAT_COUNT; m++) UnloadTexture(textures[m]);
    ready = false;
}
