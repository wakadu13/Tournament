// options.c — Lecture et écriture du fichier options.cfg (voir options.h)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"

#include "../common/protocol.h"
#include "options.h"

Options options;

#define OPTIONS_FILE "options.cfg"

// Chemin du fichier : à côté de l'exécutable sur PC. Sur Android, raylib
// écrit les chemins relatifs dans le dossier privé de l'application.
static const char *OptionsPath(void)
{
    static char path[512];
#if defined(PLATFORM_ANDROID)
    snprintf(path, sizeof(path), "%s", OPTIONS_FILE);
#else
    const char *dir = GetApplicationDirectory();
    if (dir && dir[0]) snprintf(path, sizeof(path), "%s%s", dir, OPTIONS_FILE);
    else snprintf(path, sizeof(path), "%s", OPTIONS_FILE);
#endif
    return path;
}

void OptionsDefaults(void)
{
    memset(&options, 0, sizeof(options));
    options.sensitivity = 1.0f;
    options.touch_sens = 1.0f;
    options.fov = 75.0f;
    options.invert_y = false;
    options.fullscreen = false;
    options.show_fps = true;
    options.auto_reload = true;
    options.volume = 0.8f;
    options.primary = WPN_FAMAS;
    options.secondary = WPN_GLOCK;
    snprintf(options.name, sizeof(options.name), "Joueur");
    options.server[0] = '\0';
}

static float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Une ligne « clé=valeur »
static void Apply(const char *key, const char *val)
{
    if (!strcmp(key, "sensitivity"))   options.sensitivity = ClampF((float)atof(val), 0.2f, 3.0f);
    else if (!strcmp(key, "touch_sens")) options.touch_sens = ClampF((float)atof(val), 0.2f, 3.0f);
    else if (!strcmp(key, "fov"))      options.fov = ClampF((float)atof(val), 60.0f, 100.0f);
    else if (!strcmp(key, "invert_y")) options.invert_y = atoi(val) != 0;
    else if (!strcmp(key, "fullscreen")) options.fullscreen = atoi(val) != 0;
    else if (!strcmp(key, "show_fps")) options.show_fps = atoi(val) != 0;
    else if (!strcmp(key, "auto_reload")) options.auto_reload = atoi(val) != 0;
    else if (!strcmp(key, "volume"))   options.volume = ClampF((float)atof(val), 0.0f, 1.0f);
    else if (!strcmp(key, "primary")) {
        int w = atoi(val);
        if (w >= WPN_FAMAS && w <= WPN_BENELLI) options.primary = w;
    } else if (!strcmp(key, "secondary")) {
        int w = atoi(val);
        if (w == WPN_GLOCK || w == WPN_PAMAS) options.secondary = w;
    } else if (!strcmp(key, "name")) {
        if (val[0]) snprintf(options.name, sizeof(options.name), "%s", val);
    } else if (!strcmp(key, "server")) {
        snprintf(options.server, sizeof(options.server), "%s", val);
    }
}

void OptionsLoad(void)
{
    OptionsDefaults();
    const char *path = OptionsPath();
    if (!FileExists(path)) return;
    char *text = LoadFileText(path);
    if (!text) return;

    // Une ligne à la fois : « clé=valeur », espaces et retours chariot ignorés
    char *line = text;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *cr = strchr(line, '\r');
        if (cr) *cr = '\0';
        char *eq = strchr(line, '=');
        if (eq && line[0] != '#') {
            *eq = '\0';
            Apply(line, eq + 1);
        }
        line = nl ? nl + 1 : NULL;
    }
    UnloadFileText(text);
}

bool OptionsSave(void)
{
    char text[1024];
    snprintf(text, sizeof(text),
             "# EMA Tournament — réglages du joueur\n"
             "sensitivity=%.3f\n"
             "touch_sens=%.3f\n"
             "fov=%.1f\n"
             "invert_y=%d\n"
             "fullscreen=%d\n"
             "show_fps=%d\n"
             "auto_reload=%d\n"
             "volume=%.2f\n"
             "primary=%d\n"
             "secondary=%d\n"
             "name=%s\n"
             "server=%s\n",
             options.sensitivity, options.touch_sens, options.fov,
             options.invert_y ? 1 : 0, options.fullscreen ? 1 : 0,
             options.show_fps ? 1 : 0, options.auto_reload ? 1 : 0, options.volume,
             options.primary, options.secondary, options.name, options.server);
    return SaveFileText(OptionsPath(), text);
}
