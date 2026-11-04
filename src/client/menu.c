// menu.c — Accueil, pause, équipement, options (voir menu.h)
#include <stdio.h>
#include <string.h>

#include "raylib.h"

#include "../common/protocol.h"
#include "../common/game.h"
#include "options.h"
#include "ui.h"
#include "menu.h"

static float vw = 1280.0f, vh = 720.0f;
static bool  mobile = false;

// Champ qui reçoit les frappes sur l'écran d'accueil : 0 = serveur, 1 = pseudo
static int  focus_field = 0;
// Arme mise en avant dans l'écran d'équipement (statistiques, aperçu)
static int  highlight = -1;
static bool preview_on = false;
static Rectangle preview_rect;
static int  preview_weapon = WPN_FAMAS;

static const Color ACCENT_GREEN = {70, 160, 90, 255};
static const Color ACCENT_BLUE  = {60, 110, 190, 255};
static const Color ACCENT_RED   = {180, 70, 60, 255};
static const Color ACCENT_GREY  = {90, 95, 110, 255};
static const Color ACCENT_GOLD  = {190, 140, 50, 255};

void MenuSetScreen(float w, float h, bool is_mobile)
{
    vw = w;
    vh = h;
    mobile = is_mobile;
}

bool MenuWeaponPreview(Rectangle *rect, int *weapon)
{
    if (!preview_on) return false;
    *rect = preview_rect;
    *weapon = preview_weapon;
    return true;
}

//----------------------------------------------------------------------------------
// Accueil
//----------------------------------------------------------------------------------
static MenuAction MainScreen(MenuScreen *screen, const char *status)
{
    float cx = vw / 2;
    UiTextCentered("EMA TOURNAMENT", cx, 24, 44, RAYWHITE);
    UiTextCentered("Manches à élimination sur une carte façon Dust — 9 armes", cx, 74, 18, GRAY);

    // Serveur et pseudo. Un contact sur un champ lui donne les frappes.
    Rectangle fs = { cx - 300, 124, 400, 50 };
    Rectangle fn = { cx + 120, 124, 180, 50 };
    DrawText("Serveur (adresse[:port])", (int)fs.x, 102, 18, LIGHTGRAY);
    DrawText("Pseudo", (int)fn.x, 102, 18, LIGHTGRAY);
    bool f0 = (focus_field == 0), f1 = (focus_field == 1);
    bool changed = UiTextField(fs, options.server, (int)sizeof(options.server), &f0, "127.0.0.1");
    UiTextField(fn, options.name, (int)sizeof(options.name), &f1, "Joueur");
    if (f0 && focus_field != 0) focus_field = 0;
    if (f1 && focus_field != 1) focus_field = 1;
    if (UiReleased()) {   // un contact sur un champ le prend ; sinon rien ne change
        Vector2 p = UiPointer();
        if (CheckCollisionPointRec(p, fs)) focus_field = 0;
        else if (CheckCollisionPointRec(p, fn)) focus_field = 1;
    }
    // Clavier à l'écran, sous les champs (mobile seulement)
    float by = 240;
    if (mobile) {
        Rectangle kb = { cx - 536, 196, 1072, 272 };
        char *buf = (focus_field == 0) ? options.server : options.name;
        int cap = (focus_field == 0) ? (int)sizeof(options.server) : (int)sizeof(options.name);
        if (UiKeyboard(kb, buf, cap) && focus_field == 0) changed = true;
        by = 490;
    }
    // Témoin pour le test en CI (logcat) : l'adresse après chaque frappe
    if (changed) TraceLog(LOG_INFO, "TACTILE: adresse \"%s\"", options.server);

    MenuAction a = MA_NONE;
    if (UiButton((Rectangle){ cx - 150, by, 300, 64 }, "JOUER", 30, ACCENT_GREEN, true)) a = MA_PLAY;
    if (UiButton((Rectangle){ cx + 170, by, 160, 64 }, "ARMES", 22, ACCENT_BLUE, true)) {
        *screen = MENU_WEAPONS;
        highlight = options.primary;
    }
    if (UiButton((Rectangle){ cx + 350, by, 160, 64 }, "OPTIONS", 22, ACCENT_GREY, true)) *screen = MENU_OPTIONS;
    if (UiButton((Rectangle){ cx - 330, by, 160, 64 }, "QUITTER", 22, ACCENT_RED, true)) a = MA_QUIT;

    if (status && status[0]) UiTextCentered(status, cx, by + 80, 20, (Color){235, 90, 70, 255});

    // Équipement choisi, en rappel
    char line[96];
    snprintf(line, sizeof(line), "Équipement : %s + %s", WEAPONS[options.primary].name,
             WEAPONS[options.secondary].name);
    UiTextCentered(line, cx, vh - 40, 18, GRAY);

    // Aperçu de l'arme principale, à droite (PC : il y a la place)
    preview_on = !mobile;
    if (preview_on) {
        float pw = 360, ph = 360 * vh / vw;
        preview_rect = (Rectangle){ cx + 120, by + 120, pw, ph };
        preview_weapon = options.primary;
    }
    return a;
}

//----------------------------------------------------------------------------------
// Pause
//----------------------------------------------------------------------------------
static MenuAction PauseScreen(MenuScreen *screen, const char *status)
{
    float cx = vw / 2;
    Rectangle panel = { cx - 200, 110, 400, 440 };
    UiPanel(panel, "PAUSE");
    MenuAction a = MA_NONE;
    if (UiButton((Rectangle){ cx - 160, 180, 320, 56 }, "REPRENDRE", 24, ACCENT_GREEN, true)) a = MA_RESUME;
    if (UiButton((Rectangle){ cx - 160, 250, 320, 56 }, "ARMES", 24, ACCENT_BLUE, true)) {
        *screen = MENU_WEAPONS;
        highlight = options.primary;
    }
    if (UiButton((Rectangle){ cx - 160, 320, 320, 56 }, "OPTIONS", 24, ACCENT_GREY, true)) *screen = MENU_OPTIONS;
    if (UiButton((Rectangle){ cx - 160, 390, 320, 56 }, "QUITTER LA PARTIE", 22, ACCENT_RED, true)) a = MA_QUIT;
    if (status && status[0]) UiTextCentered(status, cx, 470, 18, LIGHTGRAY);
    UiTextCentered(mobile ? "Touchez MENU pour revenir ici" : "Échap : reprendre   B : armes", cx, 510, 16, GRAY);
    preview_on = false;
    return a;
}

//----------------------------------------------------------------------------------
// Équipement
//----------------------------------------------------------------------------------
static const char *ModeName(int mode)
{
    switch (mode) {
    case FIRE_AUTO: return "automatique";
    case FIRE_SEMI: return "coup par coup";
    case FIRE_BOLT: return "à verrou";
    case FIRE_PUMP: return "à pompe";
    default: return "?";
    }
}

static MenuAction WeaponsScreen(MenuScreen *screen, bool in_game, const char *status)
{
    float cx = vw / 2;
    float pw = (vw < 1100) ? vw - 40 : 1060;
    Rectangle panel = { cx - pw / 2, 30, pw, vh - 60 };
    UiPanel(panel, "ÉQUIPEMENT");
    if (highlight < 0 || highlight >= WPN_CLAWS) highlight = options.primary;

    // Colonne de gauche : armes principales ; colonne du milieu : armes de poing
    float x0 = panel.x + 24;
    DrawText("Arme principale", (int)x0, 70, 18, LIGHTGRAY);
    float y = 94;
    for (int w = WPN_FAMAS; w <= WPN_BENELLI; w++) {
        bool sel = (options.primary == w);
        Rectangle r = { x0, y, 250, 50 };
        Color c = sel ? ACCENT_GOLD : (highlight == w ? ACCENT_BLUE : ACCENT_GREY);
        if (UiButton(r, "", 20, c, true)) { options.primary = w; highlight = w; }
        DrawText(WEAPONS[w].name, (int)(r.x + 14), (int)(r.y + 7), 20, RAYWHITE);
        DrawText(WEAPONS[w].kind, (int)(r.x + 14), (int)(r.y + 29), 14, Fade(RAYWHITE, 0.7f));
        y += 56;
    }
    float x1 = x0 + 274;
    DrawText("Arme de poing", (int)x1, 70, 18, LIGHTGRAY);
    y = 94;
    for (int w = WPN_GLOCK; w <= WPN_PAMAS; w++) {
        bool sel = (options.secondary == w);
        Rectangle r = { x1, y, 220, 50 };
        Color c = sel ? ACCENT_GOLD : (highlight == w ? ACCENT_BLUE : ACCENT_GREY);
        if (UiButton(r, "", 20, c, true)) { options.secondary = w; highlight = w; }
        DrawText(WEAPONS[w].name, (int)(r.x + 14), (int)(r.y + 7), 20, RAYWHITE);
        DrawText(WEAPONS[w].kind, (int)(r.x + 14), (int)(r.y + 29), 14, Fade(RAYWHITE, 0.7f));
        y += 56;
    }
    UiTextCentered("Touchez une arme pour", x1 + 110, 230, 14, GRAY);
    UiTextCentered("voir ses caractéristiques", x1 + 110, 248, 14, GRAY);

    // Fiche de l'arme mise en avant : statistiques et aperçu 3D
    const WeaponStats *ws = &WEAPONS[highlight];
    float x2 = x1 + 250;
    float fw = panel.x + panel.width - 24 - x2;
    DrawText(ws->name, (int)x2, 70, 30, RAYWHITE);
    char line[96];
    snprintf(line, sizeof(line), "%s — tir %s", ws->kind, ModeName(ws->mode));
    DrawText(line, (int)x2, 104, 16, LIGHTGRAY);
    float by = 134;
    UiBar((Rectangle){ x2, by, fw, 18 }, "Dégâts", (float)(ws->damage * ws->pellets), 100.0f, (Color){220, 90, 70, 255});
    UiBar((Rectangle){ x2, by + 26, fw, 18 }, "Cadence", 60.0f / ws->cooldown, 1000.0f, (Color){230, 180, 70, 255});
    UiBar((Rectangle){ x2, by + 52, fw, 18 }, "Portée", ws->range, 300.0f, (Color){90, 160, 230, 255});
    UiBar((Rectangle){ x2, by + 78, fw, 18 }, "Précision", 0.06f - ws->spread_hip, 0.06f, (Color){120, 210, 130, 255});
    UiBar((Rectangle){ x2, by + 104, fw, 18 }, "Mobilité", ws->move_mult - 0.85f, 0.25f, (Color){200, 200, 200, 255});
    snprintf(line, sizeof(line), "%d balle%s/coup de %d  ·  chargeur %d, réserve %d  ·  recharge %.1f s",
             ws->pellets, ws->pellets > 1 ? "s" : "", ws->damage, ws->mag, ws->reserve, ws->reload);
    DrawText(line, (int)x2, (int)(by + 130), 14, LIGHTGRAY);
    // L'aperçu 3D occupe le reste de la fiche (dessiné par l'appelant)
    preview_on = true;
    // Même ratio que l'écran (voir DrawWeaponPreview) : si la hauteur
    // manque, la largeur se réduit d'autant.
    float pw2 = fw, ph = fw * vh / vw;
    float avail = panel.y + panel.height - (by + 156) - 90;
    if (ph > avail) { ph = avail; pw2 = ph * vw / vh; }
    preview_rect = (Rectangle){ x2, by + 152, pw2, ph };
    preview_weapon = highlight;

    MenuAction a = MA_NONE;
    float bty = panel.y + panel.height - 76;
    if (UiButton((Rectangle){ panel.x + panel.width - 24 - 200, bty, 200, 54 }, "VALIDER", 24, ACCENT_GREEN, true))
        a = MA_LOADOUT;
    if (UiButton((Rectangle){ x0, bty, 180, 54 }, "RETOUR", 22, ACCENT_GREY, true))
        *screen = in_game ? MENU_PAUSE : MENU_MAIN;
    if (status && status[0]) DrawText(status, (int)(x0 + 200), (int)(bty + 18), 18, LIGHTGRAY);
    if (in_game)
        DrawText("Appliqué tout de suite en début de manche (20 s), sinon à la prochaine.",
                 (int)(x0 + 200), (int)(bty - 20), 14, GRAY);
    return a;
}

//----------------------------------------------------------------------------------
// Options
//----------------------------------------------------------------------------------
static MenuAction OptionsScreen(MenuScreen *screen, bool in_game)
{
    float cx = vw / 2;
    Rectangle panel = { cx - 300, 40, 600, vh - 80 };
    UiPanel(panel, "OPTIONS");
    bool changed = false;
    float x = panel.x + 24, w = panel.width - 48, y = 90;

    changed |= UiSlider((Rectangle){ x, y, w, 60 }, "Sensibilité souris", &options.sensitivity, 0.2f, 3.0f, "x%.2f");
    y += 68;
    if (mobile) {
        changed |= UiSlider((Rectangle){ x, y, w, 60 }, "Sensibilité au doigt", &options.touch_sens, 0.2f, 3.0f, "x%.2f");
        y += 68;
    }
    changed |= UiSlider((Rectangle){ x, y, w, 60 }, "Champ de vision", &options.fov, 60.0f, 100.0f, "%.0f°");
    y += 68;
    changed |= UiToggle((Rectangle){ x, y, w, 48 }, "Inverser l'axe vertical", &options.invert_y);
    y += 54;
    if (!mobile) {
        changed |= UiToggle((Rectangle){ x, y, w, 48 }, "Plein écran", &options.fullscreen);
        y += 54;
    }
    changed |= UiToggle((Rectangle){ x, y, w, 48 }, "Afficher les images par seconde", &options.show_fps);
    y += 54;
    changed |= UiToggle((Rectangle){ x, y, w, 48 }, "Recharger automatiquement à vide", &options.auto_reload);
    y += 54;

    if (UiButton((Rectangle){ cx - 100, panel.y + panel.height - 74, 200, 54 }, "RETOUR", 22, ACCENT_GREY, true))
        *screen = in_game ? MENU_PAUSE : MENU_MAIN;
    preview_on = false;
    return changed ? MA_OPTIONS_CHANGED : MA_NONE;
}

MenuAction MenuFrame(MenuScreen *screen, bool in_game, const char *status)
{
    switch (*screen) {
    case MENU_MAIN:    return MainScreen(screen, status);
    case MENU_PAUSE:   return PauseScreen(screen, status);
    case MENU_WEAPONS: return WeaponsScreen(screen, in_game, status);
    case MENU_OPTIONS: return OptionsScreen(screen, in_game);
    default:           preview_on = false; return MA_NONE;
    }
}
