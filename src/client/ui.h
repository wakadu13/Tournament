/*
 * ui.h — Widgets d'interface « immédiats » pour les menus (PC et mobile).
 *
 * Immédiat : chaque widget est une fonction appelée à chaque image, qui
 * dessine ET rend vrai quand on vient de l'activer. Pas d'arbre de
 * widgets ni de rappels : le menu est un simple bloc de code.
 *
 * Tout est en UNITÉS D'INTERFACE (voir UiCamera dans main.c : 720 de
 * haut). Le pointeur est la souris sur PC, le doigt sur mobile ; dans les
 * deux cas on agit au RELÂCHEMENT sur le widget où le contact avait été
 * posé, ce qui est le comportement d'un bouton, et ce qui marche avec
 * l'émulation souris que raylib fait du tactile sur Android.
 *
 * Le champ de texte se remplit au clavier sur PC (GetCharPressed) ; sur
 * mobile, il n'y a pas de clavier : UiKeyboard en dessine un à l'écran
 * (quatre rangées : chiffres, AZERTY, et une rangée avec le point, les
 * deux-points, le tiret et l'effacement).
 */
#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include <stdbool.h>
#include "raylib.h"

// À appeler une fois par image, avant les widgets. ui_scale = pixels
// réels par unité d'interface (les contacts arrivent en pixels réels).
void UiBegin(float ui_scale);

Vector2 UiPointer(void);       // position du pointeur, en unités d'interface
bool    UiDown(void);          // un contact est en cours
bool    UiReleased(void);      // le contact vient de se lever (cette image)
Vector2 UiPressPos(void);      // là où le contact en cours (ou qui vient de se lever) s'est posé

// Bouton : vrai quand on vient de l'activer. `accent` colore le fond.
bool UiButton(Rectangle r, const char *label, int font_size, Color accent, bool enabled);

// Case à cocher : bascule *value, vrai si elle vient de changer
bool UiToggle(Rectangle r, const char *label, bool *value);

// Curseur horizontal : *value suit le pointeur tant qu'il est posé dessus.
// `fmt` formate la valeur affichée à droite (ex. "%.1f"). Vrai si changé.
bool UiSlider(Rectangle r, const char *label, float *value, float min, float max, const char *fmt);

// Champ de texte. *focused dit s'il reçoit les frappes ; un contact sur le
// champ le prend, un contact ailleurs ne le rend pas (c'est au menu de
// gérer l'exclusivité). Vrai si le texte a changé.
bool UiTextField(Rectangle r, char *buf, int cap, bool *focused, const char *placeholder);

// Clavier à l'écran (mobile) : les touches ajoutent au tampon, « < » efface.
// Rend vrai si le tampon a changé. Dessiné dans `area` (4 rangées).
bool UiKeyboard(Rectangle area, char *buf, int cap);

// Panneau sombre avec un titre
void UiPanel(Rectangle r, const char *title);

// Barre de niveau (statistiques d'une arme) : étiquette, puis barre à value/max
void UiBar(Rectangle r, const char *label, float value, float max, Color c);

// Texte centré horizontalement dans la largeur w à partir de x
void UiTextCentered(const char *text, float cx, float y, int size, Color c);

#endif // UI_WIDGETS_H
