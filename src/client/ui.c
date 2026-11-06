// ui.c — Widgets immédiats (voir ui.h)
#include <string.h>
#include <stdio.h>

#include "raylib.h"
#include "ui.h"

static Vector2 ptr;            // pointeur, en unités d'interface
static bool    down, was_down, released;
static Vector2 press_pos;      // là où le contact a commencé
static float   scale = 1.0f;

void UiBegin(float ui_scale)
{
    scale = (ui_scale > 0.0001f) ? ui_scale : 1.0f;
    // Les deux sources : souris (PC, et émulation du tactile sur Android)
    // et points de contact (Android). Voir le commentaire de ui.h.
    int touches = GetTouchPointCount();
    down = IsMouseButtonDown(MOUSE_BUTTON_LEFT) || touches > 0;
    Vector2 raw = (touches > 0) ? GetTouchPosition(0) : GetMousePosition();
    if (down || !was_down) ptr = (Vector2){ raw.x / scale, raw.y / scale };
    if (down && !was_down) press_pos = ptr;
    released = was_down && !down;
    was_down = down;
}

Vector2 UiPointer(void)  { return ptr; }
bool    UiDown(void)     { return down; }
bool    UiReleased(void) { return released; }
Vector2 UiPressPos(void) { return press_pos; }

// Le contact s'est posé ET levé dans r
static bool Clicked(Rectangle r)
{
    return released && CheckCollisionPointRec(press_pos, r) && CheckCollisionPointRec(ptr, r);
}
static bool Hover(Rectangle r)
{
    return CheckCollisionPointRec(ptr, r);
}

void UiTextCentered(const char *text, float cx, float y, int size, Color c)
{
    DrawText(text, (int)(cx - MeasureText(text, size) / 2.0f), (int)y, size, c);
}

bool UiButton(Rectangle r, const char *label, int font_size, Color accent, bool enabled)
{
    bool hot = enabled && Hover(r);
    bool pressing = hot && down && CheckCollisionPointRec(press_pos, r);
    Color fill = enabled ? Fade(accent, pressing ? 0.95f : (hot ? 0.8f : 0.6f)) : Fade(GRAY, 0.35f);
    DrawRectangleRounded(r, 0.18f, 6, fill);
    DrawRectangleRoundedLinesEx(r, 0.18f, 6, 2, Fade(RAYWHITE, enabled ? 0.5f : 0.2f));
    UiTextCentered(label, r.x + r.width / 2, r.y + r.height / 2 - font_size / 2.0f, font_size,
                   enabled ? RAYWHITE : LIGHTGRAY);
    return enabled && Clicked(r);
}

bool UiToggle(Rectangle r, const char *label, bool *value)
{
    DrawRectangleRounded(r, 0.25f, 6, Fade(RAYWHITE, Hover(r) ? 0.16f : 0.1f));
    Rectangle box = { r.x + 12, r.y + r.height / 2 - 14, 28, 28 };
    DrawRectangleRoundedLinesEx(box, 0.3f, 4, 2, Fade(RAYWHITE, 0.7f));
    if (*value) DrawRectangleRounded((Rectangle){ box.x + 6, box.y + 6, 16, 16 }, 0.3f, 4, (Color){120, 220, 140, 255});
    DrawText(label, (int)(r.x + 54), (int)(r.y + r.height / 2 - 11), 22, RAYWHITE);
    if (Clicked(r)) { *value = !*value; return true; }
    return false;
}

bool UiSlider(Rectangle r, const char *label, float *value, float min, float max, const char *fmt)
{
    DrawRectangleRounded(r, 0.25f, 6, Fade(RAYWHITE, 0.1f));
    DrawText(label, (int)(r.x + 12), (int)(r.y + 8), 20, RAYWHITE);
    char val[32];
    snprintf(val, sizeof(val), fmt, *value);
    DrawText(val, (int)(r.x + r.width - 12 - MeasureText(val, 20)), (int)(r.y + 8), 20, LIGHTGRAY);

    // La piste, sous l'étiquette
    Rectangle track = { r.x + 14, r.y + r.height - 22, r.width - 28, 10 };
    float t = (max > min) ? (*value - min) / (max - min) : 0.0f;
    DrawRectangleRounded(track, 0.5f, 4, Fade(BLACK, 0.5f));
    DrawRectangleRounded((Rectangle){ track.x, track.y, track.width * t, track.height }, 0.5f, 4,
                         (Color){255, 200, 90, 255});
    DrawCircleV((Vector2){ track.x + track.width * t, track.y + track.height / 2 }, 11, RAYWHITE);

    // Le curseur suit le pointeur tant que le contact, posé sur le widget, dure
    if (down && CheckCollisionPointRec(press_pos, r)) {
        float nt = (ptr.x - track.x) / track.width;
        if (nt < 0) nt = 0;
        if (nt > 1) nt = 1;
        float nv = min + (max - min) * nt;
        if (nv != *value) { *value = nv; return true; }
    }
    return false;
}

bool UiTextField(Rectangle r, char *buf, int cap, bool *focused, const char *placeholder)
{
    bool changed = false;
    if (Clicked(r)) *focused = true;

    DrawRectangleRounded(r, 0.2f, 6, Fade(BLACK, 0.5f));
    DrawRectangleRoundedLinesEx(r, 0.2f, 6, 2, *focused ? (Color){255, 200, 90, 255} : Fade(RAYWHITE, 0.35f));
    bool empty = (buf[0] == '\0');
    const char *shown = empty ? placeholder : buf;
    int size = 26;
    DrawText(shown, (int)(r.x + 14), (int)(r.y + r.height / 2 - size / 2.0f), size,
             empty ? DARKGRAY : RAYWHITE);
    // Curseur clignotant
    if (*focused && ((int)(GetTime() * 2) % 2 == 0)) {
        int w = empty ? 0 : MeasureText(buf, size);
        DrawRectangle((int)(r.x + 16 + w), (int)(r.y + 10), 2, (int)(r.height - 20), RAYWHITE);
    }

    // Clavier physique (PC, ou clavier Bluetooth sur mobile)
    if (*focused) {
        int c;
        while ((c = GetCharPressed()) != 0) {
            size_t len = strlen(buf);
            if (c >= 32 && c < 127 && len + 1 < (size_t)cap) {
                buf[len] = (char)c;
                buf[len + 1] = '\0';
                changed = true;
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            size_t len = strlen(buf);
            if (len > 0) { buf[len - 1] = '\0'; changed = true; }
        }
    }
    return changed;
}

// Quatre rangées de dix touches. « < » efface, « _ » est l'espace.
static const char *KEY_ROWS[4] = { "1234567890", "AZERTYUIOP", "QSDFGHJKLM", "WXCVBN.:-<" };

bool UiKeyboard(Rectangle area, char *buf, int cap)
{
    bool changed = false;
    float gap = 8.0f;
    float kw = (area.width - 9 * gap) / 10.0f;
    float kh = (area.height - 3 * gap) / 4.0f;
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 10; col++) {
            char key = KEY_ROWS[row][col];
            Rectangle r = { area.x + col * (kw + gap), area.y + row * (kh + gap), kw, kh };
            bool pressing = down && CheckCollisionPointRec(press_pos, r);
            DrawRectangleRounded(r, 0.2f, 4, Fade(RAYWHITE, pressing ? 0.4f : 0.12f));
            DrawRectangleRoundedLinesEx(r, 0.2f, 4, 1, Fade(RAYWHITE, 0.3f));
            char label[2] = { key, '\0' };
            UiTextCentered(label, r.x + r.width / 2, r.y + r.height / 2 - 14, 28, RAYWHITE);
            if (!Clicked(r)) continue;
            size_t len = strlen(buf);
            if (key == '<') {
                if (len > 0) { buf[len - 1] = '\0'; changed = true; }
            } else if (len + 1 < (size_t)cap) {
                buf[len] = key;
                buf[len + 1] = '\0';
                changed = true;
            }
        }
    }
    return changed;
}

void UiPanel(Rectangle r, const char *title)
{
    DrawRectangleRounded(r, 0.06f, 8, Fade((Color){18, 20, 26, 255}, 0.94f));
    DrawRectangleRoundedLinesEx(r, 0.06f, 8, 2, Fade(RAYWHITE, 0.25f));
    if (title && title[0]) UiTextCentered(title, r.x + r.width / 2, r.y + 16, 32, RAYWHITE);
}

void UiBar(Rectangle r, const char *label, float value, float max, Color c)
{
    DrawText(label, (int)r.x, (int)r.y, 16, LIGHTGRAY);
    Rectangle track = { r.x + 110, r.y + 3, r.width - 110, r.height - 6 };
    float t = (max > 0) ? value / max : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    DrawRectangleRec(track, Fade(BLACK, 0.5f));
    DrawRectangleRec((Rectangle){ track.x, track.y, track.width * t, track.height }, c);
}
