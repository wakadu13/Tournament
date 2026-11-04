/*
 * menu.h — Les menus du jeu : accueil, pause, équipement, options.
 *
 * Chaque écran est une fonction « immédiate » (voir ui.h) appelée à
 * chaque image : elle dessine, lit le pointeur, et rend l'action que le
 * joueur vient de demander (jouer, reprendre, quitter, valider son
 * équipement). La navigation entre écrans se fait en changeant *screen.
 *
 * Les menus écrivent directement dans `options` (options.h) : serveur,
 * pseudo, armes choisies, sensibilité... C'est l'appelant qui sauvegarde
 * (OptionsSave) et qui applique ce qui touche au jeu en cours (envoyer
 * l'équipement au serveur, passer en plein écran).
 */
#ifndef MENU_H
#define MENU_H

#include <stdbool.h>
#include "raylib.h"

typedef enum {
    MENU_NONE = 0,     // pas de menu : on joue
    MENU_MAIN,         // accueil : serveur, pseudo, jouer
    MENU_PAUSE,        // en jeu : reprendre, armes, options, quitter
    MENU_WEAPONS,      // choix de l'arme principale et de l'arme de poing
    MENU_OPTIONS,      // réglages
} MenuScreen;

typedef enum {
    MA_NONE = 0,
    MA_PLAY,           // accueil : se connecter à options.server
    MA_RESUME,         // pause : fermer le menu
    MA_QUIT,           // quitter le jeu (accueil) ou la partie (pause)
    MA_LOADOUT,        // l'équipement (options.primary / secondary) vient d'être validé
    MA_OPTIONS_CHANGED,// un réglage vient de changer (à appliquer / sauver)
} MenuAction;

// Dimensions de l'écran en unités d'interface, et si on est sur mobile
// (clavier à l'écran, réglage de la visée au doigt).
void MenuSetScreen(float w, float h, bool mobile);

// Une image de l'écran *screen (à appeler entre BeginMode2D(UiCamera()) et
// EndMode2D, après UiBegin). in_game : les écrans reviennent au menu
// pause plutôt qu'à l'accueil. status : message affiché (erreur de
// connexion, « équipement appliqué »...), ou NULL.
MenuAction MenuFrame(MenuScreen *screen, bool in_game, const char *status);

// Où dessiner l'aperçu 3D de l'arme (en unités d'interface) et laquelle :
// vrai si l'écran courant en montre un. L'appelant le dessine lui-même,
// hors de la caméra 2D (voir DrawWeaponPreview dans main.c).
bool MenuWeaponPreview(Rectangle *rect, int *weapon);

#endif // MENU_H
