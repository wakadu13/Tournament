/*
 * touch.c — Multi-touch : un rôle par doigt, joystick flottant, visée au
 * glissement, boutons ronds. Voir touch.h pour le principe.
 *
 * Tout est exprimé en UNITÉS D'INTERFACE (720 de haut, largeur selon le
 * ratio de l'écran) : les boutons gardent la même taille relative sur une
 * tablette comme sur un téléphone.
 */
#include "touch.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "raymath.h"

// Sur PC, raylib fait passer la souris pour un doigt : les commandes
// tactiles sont donc coupées, sauf si on les force pour les mettre au point.
#if defined(PLATFORM_ANDROID) || defined(TOUCH_CONTROLS)
    #define TOUCH_ENABLED 1
#else
    #define TOUCH_ENABLED 0
#endif

#if !TOUCH_ENABLED
//--------------------------------------------------------------------------
// PC : rien du tout. Les fonctions existent pour que le client n'ait pas à
// se demander sur quelle machine il tourne, mais elles ne font rien.
//--------------------------------------------------------------------------
void TouchReset(void) {}
void TouchUpdate(float ui_scale, float screen_w, float screen_h)
{
    (void)ui_scale; (void)screen_w; (void)screen_h;
}
void TouchApply(PlayerInput *in, float ads) { (void)in; (void)ads; }
void TouchDraw(void) {}
bool TouchUsed(void) { return false; }
void TouchSetSensitivity(float mult) { (void)mult; }
void TouchSetAmmoLabel(const char *text) { (void)text; }

#else   // ===================== mobile =====================

#define MAX_FINGERS   10       // doigts suivis en même temps
#define JOY_RADIUS    78.0f    // course maximale du doigt sur le joystick
#define JOY_DEAD      0.14f    // zone morte : évite de dériver au repos
#define TOUCH_SENS    0.0045f  // radians par unité d'interface glissée
#define SPRINT_PUSH   0.85f    // joystick poussé au-delà = sprint

// Rôle d'un doigt, choisi à l'endroit où il se pose
typedef enum { TR_NONE, TR_JOY, TR_LOOK, TR_FIRE, TR_AIM, TR_JUMP, TR_RELOAD, TR_SWAP, TR_MENU, TR_COUNT } TouchRole;

// Un doigt suivi d'une image à l'autre
typedef struct {
    bool      used;      // emplacement occupé
    bool      seen;      // doigt encore présent cette image
    int       id;        // identifiant système (stable tant que le doigt est posé)
    TouchRole role;
    Vector2   origin;    // point de pose (base du joystick, ou départ du glissement)
    Vector2   pos, last; // position actuelle et position à l'image précédente
} Finger;

typedef struct { TouchRole role; Vector2 c; float r; } Button;

static Finger  fingers[MAX_FINGERS];
static Vector2 joy;            // direction du joystick, -1..1
static Vector2 joy_base;       // centre du joystick actif (unités d'interface)
static bool    joy_on;         // un doigt tient le joystick
static Vector2 look_delta;     // glissement de visée cumulé cette image
static bool    held[TR_COUNT]; // boutons maintenus cette image
static bool    pressed[TR_COUNT]; // boutons qui viennent d'être enfoncés (impulsion)
static bool    was_held[TR_COUNT];
static bool    aim_toggle;     // VISER est une bascule, pas un maintien
static bool    aim_was_down;   // pour ne basculer qu'au moment de l'appui
static float   vw = 1280.0f, vh = 720.0f;   // écran en unités d'interface
static float   sens_mult = 1.0f;            // réglage de sensibilité (options)
static char    ammo_label[24] = "ARME";     // texte du bouton de changement d'arme

// Position des boutons, calée sur le coin bas droit (TIRER sous le pouce),
// décalée de 44 unités vers la gauche : la barre de navigation Android à
// trois boutons occupe ~90 unités du bord droit et chevauchait TIRER.
// RECHARGER et ARME (changement) complètent la grappe ; MENU est en haut
// à gauche, hors de la zone du joystick (qui commence à 22 % de la hauteur).
#define BUTTON_MAX 6
static int Buttons(Button *b)
{
    int n = 0;
    b[n++] = (Button){ TR_FIRE,   { vw - 156.0f, vh - 112.0f }, 60 };
    b[n++] = (Button){ TR_AIM,    { vw - 280.0f, vh -  64.0f }, 38 };
    b[n++] = (Button){ TR_JUMP,   { vw - 108.0f, vh - 236.0f }, 36 };
    b[n++] = (Button){ TR_RELOAD, { vw - 250.0f, vh - 190.0f }, 34 };
    b[n++] = (Button){ TR_SWAP,   { vw - 108.0f, vh - 330.0f }, 32 };
    b[n++] = (Button){ TR_MENU,   { 44.0f, 44.0f }, 26 };
    return n;
}

// Rôle d'un doigt qui se pose en p :
//  - sur un bouton (zone 15 % plus large que le cercle dessiné : plus facile
//    à viser du pouce que le dessin ne le laisse croire) ;
//  - sinon dans les 40 % gauches, hors bandeau du haut : joystick ;
//  - partout ailleurs : visée.
static TouchRole HitTest(Vector2 p)
{
    Button b[BUTTON_MAX];
    int n = Buttons(b);
    for (int i = 0; i < n; i++)
        if (Vector2Distance(p, b[i].c) < b[i].r * 1.15f) return b[i].role;
    if (p.x < vw * 0.4f && p.y > vh * 0.22f) return TR_JOY;
    return TR_LOOK;
}

static Finger *FindFinger(int id)
{
    for (int i = 0; i < MAX_FINGERS; i++)
        if (fingers[i].used && fingers[i].id == id) return &fingers[i];
    return NULL;
}

static Finger *NewFinger(void)
{
    for (int i = 0; i < MAX_FINGERS; i++)
        if (!fingers[i].used) return &fingers[i];
    return NULL;
}

void TouchReset(void)
{
    memset(fingers, 0, sizeof(fingers));
    joy = (Vector2){0};
    joy_on = false;
    look_delta = (Vector2){0};
    memset(held, 0, sizeof(held));
    memset(pressed, 0, sizeof(pressed));
    memset(was_held, 0, sizeof(was_held));
    aim_toggle = false;
    aim_was_down = false;
}

void TouchSetSensitivity(float mult) { sens_mult = (mult > 0.0f) ? mult : 1.0f; }
void TouchSetAmmoLabel(const char *text)
{
    snprintf(ammo_label, sizeof(ammo_label), "%s", text ? text : "ARME");
}

bool TouchUsed(void) { return true; }

void TouchUpdate(float ui_scale, float screen_w, float screen_h)
{
    vw = screen_w;
    vh = screen_h;
    if (ui_scale < 0.0001f) ui_scale = 1.0f;

    // Repartir de zéro pour ce qui ne vaut qu'une image
    look_delta = (Vector2){0};
    memcpy(was_held, held, sizeof(held));
    memset(held, 0, sizeof(held));
    for (int i = 0; i < MAX_FINGERS; i++) fingers[i].seen = false;

    // --- Associer chaque point de contact à son doigt ------------------
    int count = GetTouchPointCount();
    if (count > MAX_FINGERS) count = MAX_FINGERS;
    for (int i = 0; i < count; i++) {
        int id = GetTouchPointId(i);
        Vector2 raw = GetTouchPosition(i);
        Vector2 p = { raw.x / ui_scale, raw.y / ui_scale };   // en unités d'interface

        Finger *f = FindFinger(id);
        if (!f) {                       // doigt qui vient de se poser
            f = NewFinger();
            if (!f) continue;
            f->used = true;
            f->id = id;
            f->role = HitTest(p);
            f->origin = p;
            f->pos = f->last = p;
        }
        f->seen = true;
        f->last = f->pos;
        f->pos = p;
    }

    // Les doigts qu'on n'a pas revus sont levés
    for (int i = 0; i < MAX_FINGERS; i++)
        if (fingers[i].used && !fingers[i].seen) memset(&fingers[i], 0, sizeof(Finger));

    // --- Traiter chaque doigt selon son rôle ---------------------------
    joy = (Vector2){0};
    joy_on = false;
    bool aim_down = false;

    for (int i = 0; i < MAX_FINGERS; i++) {
        Finger *f = &fingers[i];
        if (!f->used) continue;

        switch (f->role) {
        case TR_JOY: {
            // Le joystick est né sous le doigt : la direction est l'écart
            // au point de pose, borné à JOY_RADIUS.
            Vector2 d = Vector2Subtract(f->pos, f->origin);
            float len = Vector2Length(d);
            if (len > JOY_RADIUS) {
                // Doigt au-delà de la course : la base SUIT le doigt, pour
                // ne pas le "perdre" quand on glisse loin.
                f->origin = Vector2Subtract(f->pos, Vector2Scale(d, JOY_RADIUS / len));
                d = Vector2Scale(d, JOY_RADIUS / len);
                len = JOY_RADIUS;
            }
            joy_base = f->origin;
            joy_on = true;
            float k = len / JOY_RADIUS;
            if (k < JOY_DEAD) { joy = (Vector2){0}; break; }
            // y de l'écran va vers le BAS : vers le haut = vers l'avant
            joy.x = d.x / JOY_RADIUS;
            joy.y = -d.y / JOY_RADIUS;
            break;
        }
        case TR_LOOK:
        case TR_FIRE:
            // Un doigt posé sur TIRER peut glisser pour ajuster la visée :
            // on tire et on vise du même pouce.
            look_delta.x += f->pos.x - f->last.x;
            look_delta.y += f->pos.y - f->last.y;
            if (f->role == TR_FIRE) held[TR_FIRE] = true;
            break;
        case TR_AIM:
            aim_down = true;
            break;
        case TR_JUMP:
        case TR_RELOAD:
        case TR_SWAP:
        case TR_MENU:
            held[f->role] = true;
            break;
        default:
            break;
        }
    }
    // Impulsions : le bouton n'était pas tenu à l'image précédente
    for (int r = 0; r < TR_COUNT; r++) pressed[r] = held[r] && !was_held[r];

    // VISER est une bascule : on n'inverse qu'au moment où le doigt se pose
    if (aim_down && !aim_was_down) aim_toggle = !aim_toggle;
    aim_was_down = aim_down;
}

void TouchApply(PlayerInput *in, float ads)
{
    in->move.x += joy.x;
    in->move.y += joy.y;

    // Visée : même atténuation en ADS qu'à la souris
    float sens = TOUCH_SENS * sens_mult * (1.0f - ads * 0.35f);
    in->look.x += -look_delta.x * sens;
    in->look.y += -look_delta.y * sens;

    if (held[TR_FIRE])    in->fire = true;
    if (pressed[TR_FIRE]) in->fire_pressed = true;
    if (held[TR_JUMP])    in->jump = true;
    if (pressed[TR_RELOAD]) in->reload = true;
    if (pressed[TR_SWAP])   in->swap = true;
    if (pressed[TR_MENU])   in->menu = true;
    if (aim_toggle)    in->aim = true;
    // Sprint : joystick poussé à fond vers l'avant
    if (joy.y > SPRINT_PUSH) in->sprint = true;
}

void TouchDraw(void)
{
    // --- Joystick : couronne à la base, pastille sous le doigt ---------
    if (joy_on) {
        DrawCircleLines((int)joy_base.x, (int)joy_base.y, JOY_RADIUS,
                        Fade(RAYWHITE, 0.35f));
        Vector2 k = { joy_base.x + joy.x * JOY_RADIUS,
                      joy_base.y - joy.y * JOY_RADIUS };
        DrawCircleV(k, 30, Fade(RAYWHITE, joy.y > SPRINT_PUSH ? 0.5f : 0.3f));
    }

    // --- Boutons -------------------------------------------------------
    Button b[BUTTON_MAX];
    int n = Buttons(b);
    for (int i = 0; i < n; i++) {
        bool on = (b[i].role == TR_AIM) ? aim_toggle : held[b[i].role];
        Color fill = Fade(on ? (Color){255, 210, 90, 255} : RAYWHITE, on ? 0.45f : 0.16f);
        DrawCircleV(b[i].c, b[i].r, fill);
        DrawCircleLines((int)b[i].c.x, (int)b[i].c.y, b[i].r, Fade(RAYWHITE, 0.5f));

        const char *label = (b[i].role == TR_FIRE)   ? "TIRER"
                          : (b[i].role == TR_AIM)    ? "VISER"
                          : (b[i].role == TR_JUMP)   ? "SAUT"
                          : (b[i].role == TR_RELOAD) ? "RECH."
                          : (b[i].role == TR_SWAP)   ? ammo_label : "MENU";
        int size = (b[i].role == TR_FIRE) ? 20 : (b[i].role == TR_SWAP ? 13 : 16);
        DrawText(label, (int)b[i].c.x - MeasureText(label, size) / 2,
                 (int)b[i].c.y - size / 2, size, Fade(RAYWHITE, 0.85f));
    }
}

#endif  // TOUCH_ENABLED
