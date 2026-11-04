/*
 * models.c — Modèles 3D procéduraux des personnages.
 *
 * Portés du premier jeu (« ZEFFALIE COME BACK », dépôt
 * azerfjgpodvuirefefGB, src/render.c) : trois maillages — un cube, une
 * sphère, un cylindre — et des matrices pour les poser. Voir models.h
 * pour la convention de repères.
 *
 * Tout l'animation est pilotée par un CharAnim (un par joueur, tenu par
 * l'appelant). Aucun modèle ne lit l'horloge globale : les tics viennent
 * de CharAnim.t, qui est décalé par personnage, pour que deux poussins ne
 * clignent jamais des yeux ensemble.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "raymath.h"

#include "models.h"
#include "../common/protocol.h"
#include "weapons.h"

// Raccourci pour un Vector3 littéral. ATTENTION : on ne peut pas l'appeler
// V3 comme dans le premier jeu, game.h utilise déjà ce nom pour un TYPE.
#define P3(x, y, z) ((Vector3){ (x), (y), (z) })
#define NOROT P3(0, 0, 0)

// Durées des actions, en secondes (les champs du CharAnim descendent de 1 à 0)
#define ATTACK_TIME 0.45f    // bond de griffe
#define FLINCH_TIME 0.28f    // sursaut quand il encaisse
#define DEATH_TIME  0.85f    // effondrement
#define LAND_TIME   0.26f    // amorti d'atterrissage
#define BLINK_TIME  0.13f    // clignement
#define SHAKE_TIME  0.55f    // ébrouement

// Repères de vitesse (m/s) : au-delà de WALK_SPEED le personnage court
#define WALK_SPEED  3.6f     // allure d'un poussin zombie
#define RUN_SPEED   7.0f     // allure d'un joueur

//----------------------------------------------------------------------------------
// Les trois maillages de base
//----------------------------------------------------------------------------------
static Model mCube, mSphere, mCyl;
static bool  ready = false;

//----------------------------------------------------------------------------------
// Lot (batch) : un seul maillage par image
//----------------------------------------------------------------------------------
// Les trois maillages unitaires sont « dépliés » une fois pour toutes en
// listes de triangles (position + normale par sommet). Pendant un lot,
// chaque primitive transforme ces sommets par sa matrice monde et les
// ajoute à un grand tampon ; BatchEnd envoie le tampon au GPU et le
// dessine d'un coup. Le GPU ne reçoit que positions et couleurs : pas de
// normales, l'ombrage est CUIT dans la couleur de chaque sommet (voir
// Shade() plus bas), ce qui donne du relief sans shader.
enum { SRC_CUBE, SRC_SPHERE, SRC_CYL, SRC_COUNT };
typedef struct { Vector3 *pos, *nrm; int count; } SourceTris;
static SourceTris src[SRC_COUNT];

#define BATCH_MAX_VERTS 400000    // ~16 personnages + l'enseigne, avec de la marge
static Mesh           batch;      // maillage dynamique (positions + couleurs)
static Material       batchMat;
static float         *bpos;       // BATCH_MAX_VERTS * 3
static unsigned char *bcol;       // BATCH_MAX_VERTS * 4
static int            bused = 0;
static bool           batching = false;

// Lumière du monde pour l'ombrage cuit : de haut, légèrement de côté
static const Vector3 LIGHT_DIR = { 0.36f, 0.86f, 0.36f };

// Déplie un maillage raylib (indexé ou non) en liste de triangles
static void ExpandMesh(const Mesh *m, SourceTris *out)
{
    int n = m->triangleCount * 3;
    out->count = n;
    out->pos = (Vector3 *)MemAlloc((unsigned)n * sizeof(Vector3));
    out->nrm = (Vector3 *)MemAlloc((unsigned)n * sizeof(Vector3));
    for (int i = 0; i < n; i++) {
        int v = m->indices ? (int)m->indices[i] : i;
        out->pos[i] = P3(m->vertices[v * 3], m->vertices[v * 3 + 1], m->vertices[v * 3 + 2]);
        out->nrm[i] = m->normals
            ? P3(m->normals[v * 3], m->normals[v * 3 + 1], m->normals[v * 3 + 2])
            : P3(0, 1, 0);
    }
}

void ModelsInit(void)
{
    if (ready) return;
    // Sphère en 8 x 10 : la finesse du premier EMATournament. Avec le lot,
    // chaque sommet coûte du CPU et de la bande passante, plus la sphère
    // est fine, plus l'image est chère.
    mCube   = LoadModelFromMesh(GenMeshCube(1, 1, 1));
    mSphere = LoadModelFromMesh(GenMeshSphere(1, 8, 10));
    mCyl    = LoadModelFromMesh(GenMeshCylinder(1, 1, 12));
    ExpandMesh(&mCube.meshes[0],   &src[SRC_CUBE]);
    ExpandMesh(&mSphere.meshes[0], &src[SRC_SPHERE]);
    ExpandMesh(&mCyl.meshes[0],    &src[SRC_CYL]);

    // Le maillage du lot : tampons à la taille maximale, mis à jour à
    // chaque image (UploadMesh dynamique). Pas de texcoords ni de normales :
    // raylib leur donne une valeur par défaut.
    bpos = (float *)MemAlloc(BATCH_MAX_VERTS * 3 * sizeof(float));
    bcol = (unsigned char *)MemAlloc(BATCH_MAX_VERTS * 4);
    memset(&batch, 0, sizeof(batch));
    batch.vertexCount = BATCH_MAX_VERTS;
    batch.triangleCount = BATCH_MAX_VERTS / 3;
    batch.vertices = bpos;
    batch.colors = bcol;
    UploadMesh(&batch, true);
    batchMat = LoadMaterialDefault();
    ready = true;
}

void ModelsClose(void)
{
    if (!ready) return;
    UnloadModel(mCube);
    UnloadModel(mSphere);
    UnloadModel(mCyl);
    for (int i = 0; i < SRC_COUNT; i++) { MemFree(src[i].pos); MemFree(src[i].nrm); }
    // Le maillage du lot pointe sur nos tampons : on détache avant de libérer
    batch.vertices = NULL;
    batch.colors = NULL;
    UnloadMesh(batch);
    UnloadMaterial(batchMat);
    MemFree(bpos);
    MemFree(bcol);
    ready = false;
}

// Envoie ce qui est accumulé et le dessine en un appel
static void BatchFlush(void)
{
    if (bused == 0) return;
    UpdateMeshBuffer(batch, 0, bpos, bused * 3 * (int)sizeof(float), 0);   // positions
    UpdateMeshBuffer(batch, 3, bcol, bused * 4, 0);                        // couleurs
    batch.vertexCount = bused;
    batch.triangleCount = bused / 3;
    DrawMesh(batch, batchMat, MatrixIdentity());
    bused = 0;
}

void BatchBegin(void)
{
    if (!ready) return;
    batching = true;
    bused = 0;
}

void BatchEnd(void)
{
    if (!ready) return;
    BatchFlush();
    batching = false;
}

// Ajoute un maillage unitaire transformé par world, teinté c, avec
// l'ombrage cuit dans la couleur : 70 % d'ambiance + 30 % selon l'angle
// entre la normale et la lumière. Modéré exprès : les couleurs des
// uniformes et des plumes restent reconnaissables.
static void BatchAppend(const SourceTris *t, Matrix world, Color c)
{
    if (bused + t->count > BATCH_MAX_VERTS) BatchFlush();   // plein : on dessine et on continue
    // Les normales se transforment par la partie 3x3 de la matrice (sans
    // la translation) puis se renormalisent ; avec une mise à l'échelle non
    // uniforme ce n'est qu'approché, largement assez pour un ombrage.
    for (int i = 0; i < t->count; i++) {
        Vector3 p = Vector3Transform(t->pos[i], world);
        Vector3 n = t->nrm[i];
        Vector3 wn = {
            n.x * world.m0 + n.y * world.m4 + n.z * world.m8,
            n.x * world.m1 + n.y * world.m5 + n.z * world.m9,
            n.x * world.m2 + n.y * world.m6 + n.z * world.m10,
        };
        float len = sqrtf(wn.x * wn.x + wn.y * wn.y + wn.z * wn.z);
        float d = (len > 1e-6f) ? (wn.x * LIGHT_DIR.x + wn.y * LIGHT_DIR.y + wn.z * LIGHT_DIR.z) / len : 0.0f;
        float k = 0.70f + 0.30f * (d > 0.0f ? d : 0.0f);
        float *bp = &bpos[bused * 3];
        bp[0] = p.x; bp[1] = p.y; bp[2] = p.z;
        unsigned char *bc = &bcol[bused * 4];
        bc[0] = (unsigned char)(c.r * k);
        bc[1] = (unsigned char)(c.g * k);
        bc[2] = (unsigned char)(c.b * k);
        bc[3] = c.a;
        bused++;
    }
}

// Dessine un maillage avec sa matrice monde et sa teinte (le DrawPart du
// premier jeu). Dans un lot : accumulé, dessiné à BatchEnd. Hors lot :
// DrawModel compose model.transform avec la position et l'échelle qu'on
// lui donne ; à (0,0,0) et 1.0, c'est exactement world.
static void DrawPart(Model *m, Matrix world, Color c)
{
    if (!ready) return;   // appelé avant ModelsInit : on ne dessine rien
    if (batching) {
        const SourceTris *t = (m == &mCube) ? &src[SRC_CUBE]
                            : (m == &mSphere) ? &src[SRC_SPHERE] : &src[SRC_CYL];
        BatchAppend(t, world, c);
        return;
    }
    m->transform = world;
    DrawModel(*m, P3(0, 0, 0), 1.0f, c);
}

//----------------------------------------------------------------------------------
// Primitives (reprises telles quelles du premier jeu)
//----------------------------------------------------------------------------------
Matrix TRS(Vector3 t, Vector3 r, Vector3 s)
{
    return MatrixMultiply(MatrixMultiply(MatrixScale(s.x, s.y, s.z), MatrixRotateXYZ(r)),
                          MatrixTranslate(t.x, t.y, t.z));
}

// Cube : matrice locale TRS puis repère parent
void Box3(Matrix parent, Vector3 t, Vector3 r, Vector3 s, Color c)
{
    DrawPart(&mCube, MatrixMultiply(TRS(t, r, s), parent), c);
}

// Sphère de rayon 1 étirée en ellipsoïde de rayons "radii", sans rotation
void Ball(Matrix parent, Vector3 t, Vector3 radii, Color c)
{
    DrawPart(&mSphere, MatrixMultiply(TRS(t, NOROT, radii), parent), c);
}

// Ellipsoïde dont on donne directement les axes : la matrice "frame" a pour
// colonnes les axes x, y, z et la position t. x et y doivent être unitaires
// et perpendiculaires.
void BallAxes(Matrix parent, Vector3 t, Vector3 x, Vector3 y, Vector3 radii, Color c)
{
    Vector3 z = Vector3CrossProduct(x, y);
    Matrix frame = { x.x, y.x, z.x, t.x,
                     x.y, y.y, z.y, t.y,
                     x.z, y.z, z.z, t.z,
                     0, 0, 0, 1 };
    DrawPart(&mSphere,
             MatrixMultiply(MatrixMultiply(MatrixScale(radii.x, radii.y, radii.z), frame), parent),
             c);
}

// Cylindre de rayon "radius" reliant a et b. Le cylindre de base va de
// y = 0 à y = 1 : on l'étire à la longueur voulue, on le tourne de l'axe Y
// vers la direction a->b, puis on le place en a.
void Seg(Matrix parent, Vector3 a, Vector3 b, float radius, Color c)
{
    // Le cylindre est symétrique : on l'oriente toujours vers le haut pour
    // éviter le cas dégénéré d'une rotation d'un demi-tour.
    if (b.y < a.y) { Vector3 tmp = a; a = b; b = tmp; }
    Vector3 d = Vector3Subtract(b, a);
    float len = Vector3Length(d);
    if (len < 1e-5f) return;   // longueur nulle : rien à dessiner (et pas de division par 0)
    Quaternion q = QuaternionFromVector3ToVector3(P3(0, 1, 0), Vector3Scale(d, 1.0f / len));
    Matrix m = MatrixMultiply(MatrixMultiply(MatrixScale(radius, len, radius), QuaternionToMatrix(q)),
                              MatrixTranslate(a.x, a.y, a.z));
    DrawPart(&mCyl, MatrixMultiply(m, parent), c);
}

// Membre : un cylindre avec une sphère à chaque bout, ce qui donne des
// articulations arrondies sans trous quand le membre se plie.
void Limb(Matrix parent, Vector3 a, Vector3 b, float radius, Color c)
{
    Seg(parent, a, b, radius, c);
    Ball(parent, a, P3(radius, radius, radius), c);
    Ball(parent, b, P3(radius, radius, radius), c);
}

// Ombre "pastille" : un disque noir transparent très plat posé juste
// au-dessus du sol (y = 1 cm, pour éviter le scintillement avec le sol).
void DrawBlobShadow(Vector3 p, float radius)
{
    Seg(MatrixIdentity(), P3(p.x, 0.01f, p.z), P3(p.x, 0.012f, p.z), radius,
        (Color){ 0, 0, 0, 90 });
}

// Multiplie chaque composante RVB par k (k < 1 assombrit). L'alpha ne change pas.
Color Shade(Color c, float k)
{
    return (Color){ (unsigned char)Clamp(c.r * k, 0, 255),
                    (unsigned char)Clamp(c.g * k, 0, 255),
                    (unsigned char)Clamp(c.b * k, 0, 255), c.a };
}

//----------------------------------------------------------------------------------
// Animation : mise à jour de l'état
//----------------------------------------------------------------------------------
static float Randf(float a, float b)
{
    return a + (b - a) * (float)rand() / (float)RAND_MAX;
}

// Rapproche v de goal d'une fraction qui dépend de dt (lissage exponentiel
// approché). rate = 10 veut dire « presque arrivé en un dixième de seconde ».
static float Approach(float v, float goal, float rate, float dt)
{
    return v + (goal - v) * fminf(1.0f, dt * rate);
}

// Fenêtre « pic bref » : sin^p ne garde que des bosses courtes séparées de
// longues pauses. Sert à tous les réflexes périodiques (picorage, "piou").
static float Pulse(float x, float power)
{
    return powf(fmaxf(0.0f, sinf(x)), power);
}

// Amorti de l'effondrement : il part vite puis se pose en douceur
// (1 - (1-d)^2 : dérivée maximale au début, nulle à l'arrivée).
static float DeathEase(float d)
{
    float k = 1.0f - d;
    return 1.0f - k * k;
}

void CharAnimInit(CharAnim *a, int seed)
{
    memset(a, 0, sizeof(*a));
    // Décalage déterministe mais différent d'un personnage à l'autre : les
    // tics ne tombent jamais ensemble, et deux joueurs voient la même chose.
    float s = (float)((unsigned)seed * 2654435761u % 1000u) / 1000.0f;   // 0..1
    a->t = s * 20.0f;
    a->blink_in = 1.0f + s * 3.0f;
    a->shake_in = 4.0f + s * 9.0f;
    a->drool = s;
    a->dead_roll = (seed & 1) ? 1.0f : -1.0f;
}

void CharAnimUpdate(CharAnim *a, float dt, Vector3 from, Vector3 to, bool alive)
{
    if (dt <= 0.0f) return;
    a->t += dt;

    // --- Vitesse déduite du déplacement affiché cette image -------------
    float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    float hspeed = sqrtf(dx * dx + dz * dz) / dt;   // horizontale (marche / course)
    float vspeed = dy / dt;                         // verticale (saut / chute)

    a->move = Approach(a->move, fminf(hspeed / WALK_SPEED, 1.0f), 10.0f, dt);
    a->run  = Approach(a->run,
                       Clamp((hspeed - WALK_SPEED) / (RUN_SPEED - WALK_SPEED), 0.0f, 1.0f),
                       6.0f, dt);
    // La phase du pas avance avec la DISTANCE parcourue : la cadence suit
    // naturellement la vitesse, et s'arrête net à l'arrêt.
    a->phase += dt * hspeed * 1.6f;
    // Compression verticale du pas (deux appuis par cycle)
    a->squash = fabsf(sinf(a->phase)) * a->move;

    // --- En l'air : au-delà de 1,4 m/s de vitesse verticale on décolle ---
    float air_goal = (fabsf(vspeed) > 1.4f) ? 1.0f : 0.0f;
    float was_air = a->air;
    a->air = Approach(a->air, air_goal, air_goal > 0.5f ? 14.0f : 7.0f, dt);
    if (was_air > 0.45f && a->air <= 0.45f) a->land = 1.0f;   // il vient de se poser

    // --- Comptes à rebours des actions ----------------------------------
    a->attack = fmaxf(0.0f, a->attack - dt / ATTACK_TIME);
    a->flinch = fmaxf(0.0f, a->flinch - dt / FLINCH_TIME);
    a->land   = fmaxf(0.0f, a->land   - dt / LAND_TIME);
    a->blink  = fmaxf(0.0f, a->blink  - dt / BLINK_TIME);
    a->shake  = fmaxf(0.0f, a->shake  - dt / SHAKE_TIME);

    // --- Effondrement : il part dès que le serveur le donne pour mort ---
    if (!alive) {
        a->death = fminf(1.0f, a->death + dt / DEATH_TIME);
        // On ne meurt pas en continuant son geste : l'élan de griffade et le
        // sursaut s'éteignent en un dixième de seconde et l'effondrement
        // prend la main (sans ça, un bond interrompu et la chute
        // s'additionnent et le corps traverse le sol).
        a->attack = fmaxf(0.0f, a->attack - dt / 0.12f);
        a->flinch = fmaxf(0.0f, a->flinch - dt / 0.12f);
    } else {
        a->death = fmaxf(0.0f, a->death - dt / 0.25f);
    }

    // --- Réflexes : clignement et ébrouement, à intervalles irréguliers --
    // (purement décoratif : chaque client tire ses propres instants)
    a->blink_in -= dt;
    if (a->blink_in <= 0.0f) { a->blink = 1.0f; a->blink_in = Randf(1.6f, 5.5f); }
    a->shake_in -= dt;
    if (a->shake_in <= 0.0f) { a->shake = 1.0f; a->shake_in = Randf(7.0f, 16.0f); }

    // --- Bec : entrouvert au repos, grand ouvert quand il charge ---------
    a->gape = Approach(a->gape, 0.12f + a->run * 0.6f + a->move * 0.12f, 6.0f, dt);

    // --- Bave : s'allonge, se détache, et ça repart (dent de scie 0..1) --
    a->drool += dt * 0.5f;
    if (a->drool >= 1.0f) a->drool -= 1.0f;

    // --- Regard : la tête rattrape sa cible sans à-coup ------------------
    a->look = Approach(a->look, a->look_goal, 5.0f, dt);
}

void CharAnimAttack(CharAnim *a) { a->attack = 1.0f; }
void CharAnimHit(CharAnim *a)    { a->flinch = 1.0f; a->blink = 1.0f; }

// Il a réapparu ailleurs : le cadavre ne doit pas se relever, on efface
// l'effondrement d'un coup au lieu de le laisser redescendre.
void CharAnimTeleport(CharAnim *a)
{
    a->death = 0.0f;
    a->attack = a->flinch = a->land = 0.0f;
    a->air = a->move = a->run = 0.0f;
}

void CharAnimLookAt(CharAnim *a, Vector3 self, float yaw, Vector3 target, bool has_target)
{
    if (!has_target) { a->look_goal = 0.0f; return; }
    float dx = target.x - self.x, dz = target.z - self.z;
    if (dx * dx + dz * dz < 0.04f) { a->look_goal = 0.0f; return; }
    // Angle de la proie dans le repère du corps (yaw = 0 regarde vers +Z),
    // ramené dans -PI..PI
    float rel = atan2f(dx, dz) - yaw;
    while (rel >  PI) rel -= 2.0f * PI;
    while (rel < -PI) rel += 2.0f * PI;
    a->look_goal = Clamp(rel / 1.2f, -1.0f, 1.0f);   // débattement du cou : +/- 70 degrés
}

//----------------------------------------------------------------------------------
// Le petit poussin du premier jeu (environ 25 cm)
//----------------------------------------------------------------------------------
// Porté avec sa signature d'origine (parent, t, flap) pour rester
// interchangeable avec azerf…/src/render.c, mais nettement plus animé :
// au picorage et aux battements d'ailes s'ajoutent la respiration, le
// pivotement de la tête entre deux coups de bec, le clignement des yeux,
// l'ébrouement, l'étirement d'une aile, le grattage d'une patte, le
// battement de la queue et les petits "piou".
//
// Toutes ces animations sont pilotées par t seul (via Pulse), chacune sur
// sa propre période : décaler t d'un poussin à l'autre suffit à ce qu'ils
// ne fassent jamais la même chose en même temps.
void DrawChick(Matrix parent, float t, float flap)
{
    const Color yellow = { 255, 214, 50, 255 }, wing = { 240, 186, 30, 255 },
                orange = { 255, 135, 25, 255 };

    float peck     = Pulse(t * 1.7f, 8.0f);              // picore : pics courts, longues pauses
    float breathe  = sinf(t * 2.4f) * 0.004f;            // respiration
    float shake_w  = Pulse(t * 0.33f, 40.0f);            // fenêtre d'ébrouement (~0,4 s)
    float shake    = shake_w * sinf(t * 44.0f);          // le tremblement lui-même
    float scratch  = Pulse(t * 0.51f + 2.0f, 16.0f) * sinf(t * 17.0f);   // patte qui racle
    float blink    = Pulse(t * 0.79f + 1.1f, 48.0f);     // clignement
    float stretch  = Pulse(t * 0.29f + 4.0f, 12.0f);     // une aile s'ouvre en grand
    float chirp    = Pulse(t * 2.3f + 0.7f, 24.0f);      // "piou" : le bec s'entrouvre
    float tailwag  = sinf(t * 3.1f) * 0.22f + shake * 0.5f;
    float look     = 0.55f * sinf(t * 0.45f) * (1.0f - peck);   // tourne la tête entre deux pecs

    // Corps : bascule vers l'avant en picorant, tremble en s'ébrouant.
    // Pivot au sommet des pattes (0, 0.055, 0) : translation inverse,
    // rotation, puis retour (sinon il tournerait autour du sol).
    Matrix bodyM = MatrixMultiply(
        MatrixMultiply(MatrixTranslate(0, -0.055f, 0),
                       MatrixMultiply(MatrixRotateXYZ(P3(peck * 0.5f, 0, shake * 0.13f)),
                                      MatrixTranslate(0, 0.055f, 0))),
        parent);

    // Tête : pivot à la base du cou (0, 0.15, 0.03) ; elle plonge en
    // picorant (en plus de la bascule du corps) et pivote entre les pecs.
    Matrix headM = MatrixMultiply(
        MatrixMultiply(MatrixTranslate(0, -0.15f, -0.03f),
                       MatrixMultiply(MatrixRotateXYZ(P3(peck * 0.55f, look, shake * 0.3f)),
                                      MatrixTranslate(0, 0.15f, 0.03f + peck * 0.025f))),
        bodyM);

    // Queue : pivot au centre du corps, elle bat de gauche à droite
    Matrix tailM = MatrixMultiply(
        MatrixMultiply(MatrixTranslate(0, -0.09f, 0),
                       MatrixMultiply(MatrixRotateXYZ(P3(0, tailwag, 0)),
                                      MatrixTranslate(0, 0.09f, 0))),
        bodyM);

    Vector3 head = P3(0, 0.19f, 0.06f);

    Ball(bodyM, P3(0, 0.09f, 0),
         P3(0.1f + breathe, 0.085f + breathe, 0.11f + breathe), yellow);          // corps
    Ball(tailM, P3(0, 0.12f, -0.1f), P3(0.04f, 0.03f, 0.04f), yellow);            // queue
    Ball(headM, head, P3(0.07f, 0.068f, 0.07f), yellow);                          // tête
    Ball(headM, Vector3Add(head, P3(0, 0.07f, -0.01f)),
         P3(0.012f, 0.03f + shake_w * 0.012f, 0.012f), yellow);                   // houppette
    // Bec : s'entrouvre sur chaque "piou"
    Box3(headM, Vector3Add(head, P3(0, -0.005f - chirp * 0.004f, 0.075f)),
         P3(chirp * 0.45f, PI / 4, 0), P3(0.035f, 0.02f, 0.035f), orange);

    // Boucle symétrique : k = -1 côté gauche, k = +1 côté droit
    for (int k = -1; k <= 1; k += 2)
    {
        Ball(headM, Vector3Add(head, P3(0.04f * k, 0.015f, 0.055f)),
             P3(0.013f, 0.016f, 0.01f), BLACK);                                   // œil
        // Paupière : un repli jaune qui descend sur l'œil pendant le clignement
        if (blink > 0.02f)
            Ball(headM, Vector3Add(head, P3(0.04f * k, 0.016f, 0.0565f)),
                 P3(0.0145f, 0.018f * blink, 0.011f), yellow);

        // Ailes : angle de base 0.15 rad, plus une oscillation rapide
        // (38 rad/s, ~6 battements/s) proportionnelle à flap, plus
        // l'ébrouement, plus l'étirement qui n'ouvre QUE l'aile droite.
        float a = 0.15f + flap * (0.5f + 0.5f * sinf(t * 38.0f)) * 1.1f
                + fabsf(shake) * 0.45f
                + ((k > 0) ? stretch * 1.15f : 0.0f);
        // Charnière de l'aile : rotation autour de Z puis déplacement sur le flanc
        Matrix wm = MatrixMultiply(MatrixMultiply(MatrixRotateZ(k * a),
                                                  MatrixTranslate(0.09f * k, 0.13f, 0)),
                                   bodyM);
        Ball(wm, P3(0, -0.035f, -0.01f), P3(0.025f, 0.05f, 0.07f), wing);
        // Bout de l'aile : les plumes se découvrent quand elle s'ouvre
        Ball(wm, P3(0, -0.085f, -0.015f), P3(0.017f, 0.032f, 0.05f), Shade(wing, 0.92f));

        // Pattes (dans le repère parent : elles restent posées au sol).
        // Seule la patte droite gratte, par salves.
        float sc = (k > 0) ? scratch : 0.0f;
        Seg(parent, P3(0.035f * k, 0.0f, 0.02f), P3(0.035f * k, 0.04f, 0.0f), 0.008f, orange);
        Box3(parent, P3(0.035f * k, 0.005f + fmaxf(0.0f, sc) * 0.014f,
                        0.035f + sc * 0.028f),
             P3(sc * 0.5f, 0, 0), P3(0.03f, 0.01f, 0.05f), orange);               // pied
    }
}

//----------------------------------------------------------------------------------
// Le soldat-mercenaire (modèle des joueurs humains)
//----------------------------------------------------------------------------------
// Silhouette reprise du premier jeu : treillis olive aux manches
// retroussées, visage entièrement peint en vert aux yeux cernés de noir et
// injectés de sang, cheveux ras, rangers, démarche de zombie bras tendus
// vers l'avant. Les épaulettes et le brassard prennent la couleur de
// l'équipe pour reconnaître amis et ennemis d'un coup d'œil.
void DrawSoldier(Vector3 pos, float yaw, const CharAnim *an, Color team_color, int weapon)
{
    // Palette (reprise du premier jeu)
    const Color UNIFORM = {88, 96, 68, 255};     // treillis olive
    const Color PANTS   = Shade(UNIFORM, 0.8f);  // pantalon plus sombre
    const Color SKIN    = {222, 172, 140, 255};  // peau des avant-bras
    const Color PAINT   = {112, 126, 80, 255};   // visage peint en vert
    const Color PAINT_BLACK = {24, 26, 22, 255}; // cernes noirs
    const Color EYE_WHITE = {238, 212, 206, 255};// yeux injectés
    const Color HAIR    = {50, 38, 28, 255};     // cheveux ras
    const Color BOOTS   = {28, 26, 24, 255};     // rangers
    const Color BLOOD   = {168, 24, 20, 255};    // taches sur le brassard

    // Balancement de marche : bras et jambes en opposition, amplitude
    // proportionnelle à move (immobile = 0)
    // Comme pour le poussin : une fois mort, toutes les animations de
    // vivant s'éteignent (live) et il ne reste que l'effondrement.
    float d    = DeathEase(an->death);
    float live = 1.0f - d;
    float moving = an->move * live;
    float run  = an->run * live;
    float sw = sinf(an->phase) * moving;
    // Petit rebond vertical à chaque pas (|sin| = deux bosses par cycle)
    float bob = an->squash * 0.035f * live;
    float fl  = an->flinch * live;          // sursaut
    // Respiration : visible seulement à l'arrêt
    float breathe = sinf(an->t * 2.0f) * 0.008f * (1.0f - moving) * live;

    // Repère monde : on tourne puis on place (les pièces sont ensuite
    // décrites dans le repère local du soldat)
    Matrix world = MatrixMultiply(MatrixRotateY(yaw), MatrixTranslate(pos.x, pos.y, pos.z));

    // Mort : il part en arrière sur le côté, pivot aux pieds, et s'enfonce
    Matrix stance = MatrixMultiply(
        MatrixMultiply(MatrixRotateXYZ(P3(-d * 1.4f, 0, an->dead_roll * d * 0.4f)),
                       MatrixTranslate(0, -d * 0.1f, 0)),
        world);

    // --- Jambes (sous le rebond : elles restent posées au sol) ---
    for (int k = -1; k <= 1; k += 2) {   // k = -1 : droite, k = +1 : gauche
        float swing = sw * k * 0.24f * (1.0f + run * 0.5f);      // le pied avance ou recule
        float lift = fmaxf(0, -swing) * 0.18f + an->air * live * 0.2f;  // il se soulève en reculant
        float buckle = d * 0.35f + an->land * live * 0.1f;        // genoux qui plient
        Vector3 hip = {0.1f * k, 0.95f - buckle * 0.5f, 0};
        Vector3 foot = {0.12f * k, 0.1f + lift + buckle * 0.3f, swing};
        // Genou à mi-chemin, un peu en avant : la jambe plie dans le bon sens
        Vector3 knee = {(hip.x + foot.x) / 2, 0.52f + lift * 0.5f - buckle * 0.2f,
                        (hip.z + foot.z) / 2 + 0.05f + buckle * 0.2f};
        Limb(stance, hip, knee, 0.085f, PANTS);          // cuisse
        Seg(stance, knee, P3(foot.x, foot.y + 0.12f, foot.z), 0.075f, PANTS); // tibia
        Seg(stance, foot, P3(foot.x, foot.y + 0.16f, foot.z), 0.072f, BOOTS); // tige de la botte
        Box3(stance, P3(foot.x, foot.y - 0.05f, foot.z + 0.04f), NOROT,
             P3(0.11f, 0.1f, 0.26f), BOOTS);             // le pied
    }

    // Tout le haut du corps rebondit à la marche, respire à l'arrêt, et
    // encaisse vers l'arrière quand il est touché.
    Matrix body = MatrixMultiply(
        MatrixMultiply(MatrixRotateXYZ(P3(-fl * 0.3f + run * 0.12f, 0, 0)),
                       MatrixTranslate(0, bob + breathe - an->land * live * 0.08f, -fl * 0.06f)),
        stance);

    // --- Bassin, ceinturon ---
    Ball(body, P3(0, 0.98f, 0), P3(0.185f, 0.12f, 0.125f), PANTS);
    Ball(body, P3(0, 1.04f, 0), P3(0.19f, 0.035f, 0.132f), (Color){24, 24, 24, 255});

    // --- Torse : trois ellipsoïdes (taille, poitrine, épaules) ---
    Ball(body, P3(0, 1.1f, 0), P3(0.19f, 0.17f, 0.13f), UNIFORM);
    Ball(body, P3(0, 1.33f, 0.012f),
         P3(0.215f + breathe * 1.5f, 0.2f, 0.14f + breathe * 2.0f), UNIFORM);
    Ball(body, P3(0, 1.48f, 0), P3(0.25f, 0.09f, 0.13f), UNIFORM);

    // --- L'arme, épaulée devant la poitrine. Son repère a le canon vers
    // -Z et le soldat regarde vers +Z : on la retourne, on la pose un peu
    // à droite, et chaque coup (attack) la fait reculer vers l'épaule. Les
    // mains vont se poser sur sa poignée et son garde-main. ---
    bool armed = (weapon >= 0 && weapon < WPN_CLAWS);
    float rec = an->attack * live;
    Matrix gun = MatrixMultiply(
        MatrixMultiply(MatrixRotateY(PI), MatrixRotateX(-0.08f + rec * 0.1f)),
        MatrixTranslate(0.11f, 1.3f + sw * 0.01f, 0.26f - rec * 0.05f));
    if (armed) {
        WeaponPose wp = {0};
        wp.muzzle = (an->attack > 0.85f) ? 1.0f : 0.0f;   // flash juste au coup
        wp.bolt = rec;
        DrawWeaponModel(MatrixMultiply(gun, body), weapon, &wp);
    }
    Vector3 grip_r = {0.21f, 1.44f, 0.54f}, grip_l = {-0.21f, 1.44f, 0.54f};
    if (armed) {
        Vector3 r, l;
        WeaponGrips(weapon, &r, &l);
        grip_r = Vector3Transform(r, gun);
        grip_l = Vector3Transform(l, gun);
    }

    // --- Bras : sur l'arme s'il en a une, sinon tendus vers l'avant,
    // démarche de zombie (comme le premier jeu), balançant doucement en
    // opposition à la marche ---
    for (int k = -1; k <= 1; k += 2) {
        float s = sw * k;
        Vector3 shoulder = {0.235f * k, 1.47f, 0};
        Vector3 hand, elbow;
        if (armed) {
            // Le bras du côté +X (celui de la crosse, l'arme étant décalée
            // en +X) tient la poignée ; l'autre tient le garde-main.
            hand = (k > 0) ? grip_r : grip_l;
            // Le coude tombe sous la ligne épaule-main, vers l'extérieur
            elbow = Vector3Lerp(shoulder, hand, 0.5f);
            elbow.x += 0.1f * k;
            elbow.y -= 0.13f;
            elbow.z -= 0.03f;
        } else {
            elbow = (Vector3){0.26f * k, 1.42f + s * 0.02f, 0.27f + s * 0.04f};
            hand = (Vector3){0.21f * k, 1.44f + s * 0.05f, 0.54f + s * 0.06f};
        }
        // Manche retroussée : treillis jusqu'à 82 % du coude, revers roulé
        Vector3 cuff = Vector3Lerp(shoulder, elbow, 0.82f);
        Limb(body, shoulder, cuff, 0.062f, UNIFORM);
        Seg(body, cuff, Vector3Lerp(shoulder, elbow, 0.95f), 0.07f, Shade(UNIFORM, 0.9f));
        // Avant-bras nu et main
        Limb(body, elbow, hand, 0.042f, SKIN);
        Ball(body, hand, P3(0.045f, 0.055f, 0.04f), SKIN);
        // Épaulette à la couleur de l'équipe, inclinée avec l'épaule
        Box3(body, P3(0.15f * k, 1.562f, 0), P3(0, 0, -0.27f * k),
             P3(0.1f, 0.02f, 0.07f), team_color);
    }
    // Brassard d'équipe sur le bras gauche, taché de sang
    Seg(body, P3(0.245f, 1.455f, 0.08f), P3(0.255f, 1.44f, 0.17f), 0.071f, team_color);
    Ball(body, P3(0.3f, 1.45f, 0.12f), P3(0.012f, 0.022f, 0.014f), BLOOD);
    Ball(body, P3(0.29f, 1.44f, 0.16f), P3(0.012f, 0.016f, 0.02f), BLOOD);

    // --- Tête : visage entièrement peint en vert. Pivot à la base du cou :
    // elle suit sa cible du regard, encaisse en arrière, retombe à la mort.
    Matrix head = MatrixMultiply(
        MatrixMultiply(MatrixTranslate(0, -1.5f, 0),
                       MatrixMultiply(MatrixRotateXYZ(P3(-fl * 0.35f + d * 0.5f,
                                                         an->look * live * 0.8f, d * 0.3f)),
                                      MatrixTranslate(0, 1.5f, 0))),
        body);

    Seg(body, P3(0, 1.5f, -0.005f), P3(0, 1.68f, 0.01f), 0.052f, PAINT);          // cou
    Ball(head, P3(0, 1.69f, 0.025f), P3(0.085f, 0.06f, 0.085f), PAINT);           // mâchoire
    Ball(head, P3(0, 1.775f, 0.012f), P3(0.105f, 0.13f, 0.115f), PAINT);          // crâne
    for (int k = -1; k <= 1; k += 2)   // oreilles (peau, pas peintes)
        Ball(head, P3(0.103f * k, 1.765f, -0.005f), P3(0.018f, 0.04f, 0.028f), SKIN);
    // Cheveux : rasés sur les côtés (calotte sombre), plus fournis dessus
    Ball(head, P3(0, 1.79f, -0.02f), P3(0.108f, 0.118f, 0.1f), Shade(HAIR, 0.75f));
    Ball(head, P3(0, 1.86f, 0.008f), P3(0.092f, 0.06f, 0.1f), HAIR);

    // Yeux : cerne noir, blanc injecté de sang, pupille sombre ; la
    // paupière descend sur le clignement et reste fermée une fois mort.
    float lid = fmaxf(an->blink, an->death);
    for (int k = -1; k <= 1; k += 2) {
        Ball(head, P3(0.037f * k, 1.787f, 0.1f), P3(0.03f, 0.024f, 0.012f), PAINT_BLACK);
        Ball(head, P3(0.037f * k, 1.787f, 0.106f), P3(0.024f, 0.017f, 0.012f), EYE_WHITE);
        Ball(head, P3(0.037f * k, 1.787f, 0.115f), P3(0.009f, 0.01f, 0.005f),
             (Color){40, 28, 20, 255});
        if (lid > 0.02f)
            Ball(head, P3(0.037f * k, 1.788f, 0.108f),
                 P3(0.026f, 0.019f * lid, 0.014f), PAINT);
        // Coulure de peinture noire sous l'œil
        Ball(head, P3(0.036f * k, 1.74f, 0.105f), P3(0.012f, 0.045f, 0.008f), PAINT_BLACK);
    }
    // Nez et bouche (la bouche s'ouvre quand il court : il est à bout de souffle)
    Ball(head, P3(0, 1.76f, 0.122f), P3(0.018f, 0.03f, 0.022f), Shade(PAINT, 1.05f));
    Ball(head, P3(0, 1.705f - run * 0.008f, 0.105f),
         P3(0.035f, 0.008f + run * 0.014f, 0.012f), (Color){60, 18, 18, 255});
}

//----------------------------------------------------------------------------------
// Le poussin zombie (modèle des bots)
//----------------------------------------------------------------------------------
/*
 * Un poussin humanoïde d'1m60, jaune sale, couvert de sang, les orbites
 * noires et vides percées d'une lueur rouge, le bec grand ouvert qui
 * dégouline, les ailes-bras tendues vers sa proie.
 *
 * Il est monté comme un vrai squelette, chaque pièce dans le repère de son
 * parent (voir models.h) :
 *
 *   stance ... le sol sous lui : il porte les pattes et encaisse la chute
 *     body ... le tronc : dandinement, compression du pas, bond, respiration
 *       neck/head ... pivot du cou : tic d'épouvante, regard, plongeon
 *         hinge ... charnière du bec
 *           upper / lower ... les deux mandibules (le bec CLAQUE)
 *             drool ... la bave qui s'allonge sous la mandibule basse
 *       wingL / wingR ... charnière d'épaule : battement + écartement
 *       tail ... pivot de la queue : elle bat
 *
 * Les couches d'animation qui s'additionnent là-dessus :
 *   marche      balancement des pattes et contre-balancement des ailes,
 *               la foulée s'allonge et le corps se penche à la course ;
 *   dandinement roulis du tronc, une fois par deux pas ;
 *   compression le tronc s'écrase un peu à chaque appui ;
 *   respiration le tronc se gonfle, surtout à l'arrêt ;
 *   repos       léger transfert de poids d'une patte sur l'autre ;
 *   tic         la tête penche lentement puis se redresse d'un coup ;
 *   regard      le cou suit sa proie (CharAnimLookAt) ;
 *   clignement  une paupière descend sur l'orbite ;
 *   ébrouement  il secoue ses plumes de la tête à la queue ;
 *   bec         entrouvert au repos, il claque des mandibules, s'ouvre en
 *               grand quand il charge ;
 *   bave        un filet s'allonge sous le bec, se détache et tombe ;
 *   griffade    bond en avant, bec grand ouvert qui CLAQUE au contact,
 *               ailes jetées devant, doigts crispés, lueur des yeux qui monte ;
 *   sursaut     il part en arrière quand il encaisse une balle ;
 *   saut        pattes repliées, ailes qui battent vite, atterrissage amorti ;
 *   mort        il bascule sur le côté, les pattes se dérobent, la tête
 *               retombe, les ailes s'écartent et les paupières se ferment.
 */
void DrawZombieChick(Vector3 pos, float yaw, const CharAnim *an, Color team_color)
{
    // Jaune terne de plumes sales, et ses déclinaisons
    // (FEATHER/BEAK plutôt que YELLOW/ORANGE : ce sont des macros raylib)
    const Color FEATHER = {208, 178, 70, 255};
    const Color WING   = Shade(FEATHER, 0.88f);
    const Color BEAK   = {230, 120, 25, 255};  // bec et pattes
    const Color BLOOD  = {150, 18, 14, 255};   // sang séché
    const Color BLOOD2 = {190, 30, 22, 255};   // sang frais
    const Color SOCKET = {12, 10, 10, 255};    // orbites vides
    const Color GLOW   = {255, 40, 30, 255};   // lueur rouge au fond

    //-- Les valeurs d'animation, toutes tirées du CharAnim ----------------
    // d = avancement de l'effondrement, live = ce qu'il lui reste de vie.
    // TOUTES les animations de vivant sont atténuées par live ici, une fois
    // pour toutes : un cadavre ne court pas, ne se dandine pas, ne s'ébroue
    // pas et n'écarte pas les ailes. C'est aussi ce qui empêche la chute et
    // un geste en cours de s'additionner — une fois couché, l'axe X du
    // poussin pointe vers le haut, et une aile écartée passerait sous le sol.
    float d    = DeathEase(an->death);
    float live = 1.0f - d;

    float moving = an->move * live;
    float run    = an->run * live;
    float air    = an->air * live;
    float land   = an->land * live;
    float fl     = an->flinch * live;                       // sursaut

    float sw     = sinf(an->phase) * moving;                // balancement des pattes
    float stride = 1.0f + run * 0.55f;                      // foulée allongée à la course
    float waddle = sinf(an->phase * 0.5f) * 0.11f * moving * (1.0f + run * 0.4f);
    float bob    = an->squash * 0.05f * live;               // compression de l'appui
    float breathe = sinf(an->t * 2.6f) * (0.012f + 0.022f * (1.0f - moving)) * live;
    float idle   = sinf(an->t * 0.9f) * 0.045f * (1.0f - moving) * live;   // transfert de poids
    float look   = an->look * live;                         // mort, il ne fixe plus rien

    // Ébrouement : une oscillation rapide qui s'éteint (5 secousses)
    float shake = (an->shake > 0.0f)
                ? sinf((1.0f - an->shake) * PI * 10.0f) * an->shake * live : 0.0f;

    // Tic d'épouvante : la tête penche lentement sur 85 % du cycle puis se
    // redresse d'un coup sur les 15 % restants (dent de scie inversée).
    float tic = an->t * 0.31f;
    tic -= floorf(tic);
    float twitch = ((tic < 0.85f) ? tic / 0.85f : 1.0f - (tic - 0.85f) / 0.15f) * live;

    // Griffade : ap va de 0 à 1 pendant le bond, lunge fait l'aller-retour
    float ap    = 1.0f - an->attack;
    float lunge = sinf(ap * PI) * live;
    // Le bec s'ouvre en grand sur l'élan (45 % du bond) puis CLAQUE d'un
    // coup sec au contact
    float snap  = ((ap < 0.45f) ? (ap / 0.45f) * 0.95f
                                : fmaxf(0.0f, 0.95f - (ap - 0.45f) / 0.14f * 0.95f)) * live;
    // Claquement de mandibules au repos, et ouverture générale. Le bec reste
    // entrouvert une fois mort (d * 0.25) : c'est le seul geste qui survit.
    float chatter = sinf(an->t * 11.0f) * 0.04f * (1.0f - moving) * live;
    float jaw = an->gape * 0.5f * live + snap + chatter + fabsf(shake) * 0.12f + d * 0.25f;

    // Pattes repliées : en l'air, à l'atterrissage, et quand elles se dérobent
    float tuck = air * 0.3f + d * 0.45f + land * 0.12f;

    //-- Les repères -------------------------------------------------------
    Matrix world = MatrixMultiply(MatrixRotateY(yaw), MatrixTranslate(pos.x, pos.y, pos.z));

    // Mort : il bascule sur le flanc, du côté tiré au sort. Le pivot est
    // aux pattes, donc une bascule de 80 degrés suffit à le coucher ; on le
    // RELÈVE ensuite de 40 cm (la moitié de son épaisseur) pour qu'il repose
    // SUR le sol au lieu de passer à travers. La tête, elle, retombe par sa
    // propre rotation (voir plus bas), pas par celle du corps.
    Matrix stance = MatrixMultiply(
        MatrixMultiply(MatrixRotateXYZ(P3(d * 0.12f, 0, an->dead_roll * d * 1.4f)),
                       MatrixTranslate(0, d * 0.4f, 0)),
        world);

    // Tronc : penché à la course et pendant le bond, redressé en arrière
    // quand il encaisse ; il se dandine, s'écrase à l'appui et respire.
    // (1 - d) : un corps qui s'effondre ne garde ni penchant de course ni
    // élan de bond — seule la chute compte.
    float lean = run * 0.26f + lunge * 0.4f - fl * 0.32f;
    Matrix body = MatrixMultiply(
        MatrixMultiply(MatrixRotateXYZ(P3(lean, 0, waddle + shake * 0.17f + idle)),
                       MatrixTranslate(0, -bob + breathe + lunge * 0.13f - land * 0.14f,
                                       lunge * 0.36f - fl * 0.13f)),
        stance);

    // Cou : pivot à la base du crâne, dans le repère du tronc
    Matrix neck = MatrixMultiply(MatrixTranslate(0, 1.32f, 0.1f), body);
    Matrix head = MatrixMultiply(
        MatrixRotateXYZ(P3(twitch * 0.45f + lunge * 0.45f - fl * 0.5f + d * 0.55f
                             + run * 0.18f,
                           look * 1.1f - shake * 0.3f,
                           twitch * 0.5f + shake * 0.45f + an->dead_roll * d * 0.35f)),
        neck);

    // Charnière du bec, au bout de la tête : les deux mandibules tournent
    // autour d'elle en sens opposés (angle positif = vers le bas).
    Matrix hinge = MatrixMultiply(MatrixTranslate(0, 0.12f, 0.28f), head);
    Matrix upper = MatrixMultiply(MatrixRotateX(-jaw * 0.40f), hinge);
    Matrix lower = MatrixMultiply(MatrixRotateX( jaw * 0.95f), hinge);

    // Queue : pivot au centre du tronc, elle bat au rythme des pas
    Matrix tail = MatrixMultiply(
        MatrixMultiply(MatrixTranslate(0, -0.9f, 0),
                       MatrixMultiply(MatrixRotateXYZ(P3(-d * 0.3f,
                                                         sinf(an->phase) * 0.35f * moving
                                                           + shake * 0.55f, 0)),
                                      MatrixTranslate(0, 0.9f, 0))),
        body);

    //-- Pattes griffues (orange sale), dans le repère du sol --------------
    for (int k = -1; k <= 1; k += 2) {
        float swing = sw * k * 0.24f * stride;
        float step  = fmaxf(0.0f, -swing) * 0.35f;      // le pied se lève en revenant
        Vector3 hip  = {0.16f * k, 0.6f - tuck * 0.22f, 0};
        Vector3 foot = {0.19f * k, 0.07f + step + tuck * 0.45f, swing};
        // Genou à mi-chemin, poussé vers l'avant : la patte plie à l'envers,
        // comme chez un oiseau
        Vector3 knee = {(hip.x + foot.x) * 0.5f,
                        (hip.y + foot.y) * 0.5f - 0.03f,
                        (hip.z + foot.z) * 0.5f + 0.09f + tuck * 0.14f};
        Limb(stance, hip, knee, 0.062f, BEAK);
        Limb(stance, knee, foot, 0.05f, Shade(BEAK, 0.95f));
        // Pied en éventail : trois doigts griffus. Ils s'écartent quand le
        // pied décolle et se crispent au moment du bond.
        float splay = 0.3f + step * 1.4f + lunge * 0.55f;
        for (int toe = -1; toe <= 1; toe++)
            Box3(stance, P3(foot.x + toe * 0.06f, foot.y - 0.035f, foot.z + 0.14f),
                 P3(0, toe * splay, 0), P3(0.045f, 0.045f, 0.24f), BEAK);
    }

    //-- Corps dodu + queue ------------------------------------------------
    Ball(body, P3(0, 0.82f, 0),
         P3(0.42f + breathe, 0.4f + breathe * 0.7f, 0.46f + breathe), FEATHER);
    Ball(tail, P3(0, 1.0f, -0.42f), P3(0.16f, 0.12f, 0.16f), FEATHER);
    // Plumes de la queue (elles suivent le pivot, donc elles balaient)
    for (int f = -1; f <= 1; f++)
        Box3(tail, P3(f * 0.07f, 1.04f, -0.56f), P3(0.22f, f * 0.28f, 0),
             P3(0.04f, 0.1f, 0.22f), Shade(FEATHER, 0.92f));
    // Cou charnu : bouche le creux entre le tronc et la tête quand elle tourne
    Seg(body, P3(0, 1.08f, 0.03f), P3(0, 1.36f, 0.07f), 0.14f, FEATHER);
    // Ventre éventré : grande tache de sang et entailles sombres
    Ball(body, P3(0.1f, 0.78f, 0.4f), P3(0.16f, 0.2f, 0.05f), BLOOD);
    Ball(body, P3(0.12f, 0.72f, 0.43f), P3(0.05f, 0.12f, 0.03f), (Color){60, 8, 8, 255});
    Ball(body, P3(-0.2f, 0.95f, 0.33f), P3(0.1f, 0.08f, 0.05f), BLOOD);
    Ball(body, P3(0.28f, 0.6f, 0.28f), P3(0.07f, 0.1f, 0.05f), BLOOD);

    //-- Collier à la couleur de l'équipe (pour reconnaître son camp) ------
    Ball(body, P3(0, 1.16f, 0.05f), P3(0.26f, 0.035f, 0.26f), team_color);

    //-- Ailes-bras tendues vers l'avant, comme un zombie ------------------
    // Elles pointent vers +Z : les lever se fait donc autour de X (beat) et
    // les écarter autour de Y (spread), pas autour de Z.
    float beat = -0.1f * (1.0f - moving) * live              // elles pendent au repos
               + 0.09f * sinf(an->phase + PI) * moving       // contre-balancement du pas
               + run * 0.32f * sinf(an->phase * 2.0f)        // elles battent à la course
               + air * (0.5f + 0.4f * sinf(an->t * 26.0f))   // et vite en l'air
               + fabsf(shake) * 0.7f
               + lunge * 0.5f + fl * 0.45f
               - d * 0.5f;                                   // mort : elles retombent
    float spread = (0.12f + run * 0.18f + fabsf(shake) * 0.3f + lunge * 0.28f) * live
                 + d * 0.1f;

    for (int k = -1; k <= 1; k += 2) {
        Matrix wing = MatrixMultiply(
            MatrixMultiply(MatrixRotateX(-beat), MatrixRotateY(k * spread)),
            MatrixMultiply(MatrixTranslate(0.38f * k, 1.0f, 0.1f), body));
        Vector3 tip = {0, -0.08f, 0.52f};
        Limb(wing, P3(0, 0, 0), tip, 0.09f, WING);
        // Bout de l'aile taché de sang + trois "griffes" de plumes
        Ball(wing, tip, P3(0.1f, 0.08f, 0.1f), BLOOD2);
        for (int f = -1; f <= 1; f++)
            Box3(wing, P3(tip.x + f * 0.05f, tip.y, tip.z + 0.12f),
                 P3(0, f * 0.25f, 0), P3(0.03f, 0.03f, 0.16f), WING);
    }

    //-- Tête --------------------------------------------------------------
    Ball(head, P3(0, 0.22f, 0.03f), P3(0.34f, 0.32f, 0.34f), FEATHER);
    // Houppette : trois plumes dressées, qui se hérissent à l'ébrouement
    float crest = 1.0f + fabsf(shake) * 0.5f;
    Ball(head, P3(0, 0.56f, -0.02f), P3(0.016f, 0.09f * crest, 0.016f), FEATHER);
    Ball(head, P3(0.05f, 0.54f, 0.0f), P3(0.014f, 0.07f * crest, 0.014f), FEATHER);
    Ball(head, P3(-0.04f, 0.53f, 0.03f), P3(0.014f, 0.06f * crest, 0.014f), FEATHER);
    // Sang qui a coulé du sommet du crâne
    Ball(head, P3(0.1f, 0.42f, 0.15f), P3(0.1f, 0.12f, 0.05f), BLOOD);

    // Orbites noires et vides, percées d'une lueur rouge, larmes de sang.
    // La lueur monte quand il se jette sur sa proie ; la paupière descend
    // au clignement et reste fermée une fois mort.
    float lid = fmaxf(an->blink, an->death);
    float fire = 0.028f + lunge * 0.016f;
    for (int k = -1; k <= 1; k += 2) {
        Ball(head, P3(0.14f * k, 0.28f, 0.27f), P3(0.075f, 0.09f, 0.05f), SOCKET);
        Ball(head, P3(0.14f * k, 0.28f, 0.3f), P3(fire, fire, 0.02f), GLOW);
        Ball(head, P3(0.14f * k, 0.13f, 0.3f), P3(0.02f, 0.1f, 0.012f), BLOOD2);
        if (lid > 0.02f)
            Ball(head, P3(0.14f * k, 0.285f, 0.285f),
                 P3(0.08f, 0.095f * lid, 0.055f), Shade(FEATHER, 0.78f));
    }

    //-- Bec : deux mandibules articulées, gosier sombre, bave -------------
    Box3(upper, P3(0, 0.05f, 0.12f), P3(-0.25f, PI / 4, 0),
         P3(0.16f, 0.05f, 0.16f), BEAK);                      // demi-bec haut
    Box3(lower, P3(0, -0.06f, 0.09f), P3(0.35f, PI / 4, 0),
         P3(0.14f, 0.04f, 0.14f), Shade(BEAK, 0.85f));        // demi-bec bas
    Ball(head, P3(0, 0.11f, 0.33f), P3(0.07f, 0.05f + jaw * 0.05f, 0.05f),
         (Color){40, 6, 6, 255});                             // gosier sombre
    // Bave : un filet qui s'allonge sous la mandibule basse, se détache,
    // tombe, et ça repart (an->drool est une dent de scie 0..1).
    float len = 0.05f + an->drool * 0.13f;
    Ball(lower, P3(0.02f, -0.1f - len, 0.1f), P3(0.016f, len, 0.012f), BLOOD2);
    if (an->drool > 0.82f)   // la goutte détachée, en chute libre sous le bec
        Ball(head, P3(0.02f, -0.02f - (an->drool - 0.82f) * 9.0f, 0.38f),
             P3(0.022f, 0.022f, 0.022f), BLOOD2);
}
