/*
 * touch.h — Commandes tactiles (Android), et structure de commandes du joueur.
 *
 * Reprises de celles du premier jeu (azerfjgpodvuirefefGB, src/touch.c) :
 *   - 40 % gauche de l'écran : joystick virtuel, il apparaît SOUS le doigt
 *     là où on se pose ; poussé à fond vers l'avant = sprint ;
 *   - partout ailleurs : glisser pour viser ;
 *   - boutons ronds en bas à droite : TIRER (on peut aussi viser en
 *     glissant depuis ce bouton), VISER (bascule) et SAUTER.
 *
 * Chaque doigt reçoit un RÔLE au moment où il se pose, selon l'endroit
 * touché, et le garde jusqu'à ce qu'il soit levé : on peut donc se
 * déplacer, viser et tirer en même temps avec trois doigts.
 *
 * Sur PC ce module ne fait rien du tout (voir TOUCH_ENABLED dans touch.c) :
 * raylib fait passer la souris pour un doigt, ce qui perturberait la visée.
 * Compiler avec -DTOUCH_CONTROLS=1 force les commandes tactiles sur PC,
 * pour les mettre au point sans téléphone.
 */
#ifndef TOUCH_H
#define TOUCH_H

#include <stdbool.h>
#include "raylib.h"

/*
 * Commandes du joueur pour une image, d'où qu'elles viennent (clavier +
 * souris, ou tactile). Le client les remplit puis les applique : un seul
 * chemin de code pour le déplacement et le tir, quelle que soit la machine.
 */
typedef struct {
    Vector2 move;      // -1..1 : x = vers la droite, y = vers l'avant
    Vector2 look;      // rotation demandée cette image, en RADIANS (x = yaw, y = pitch)
    bool    fire;      // tirer
    bool    aim;       // viser (point rouge)
    bool    jump;      // sauter
    bool    sprint;    // courir
} PlayerInput;

void TouchReset(void);   // oublie tous les doigts (début de partie)

// À appeler une fois par image, AVANT de lire les commandes.
// ui_scale = pixels réels par unité d'interface ; vw / vh = taille de
// l'écran en unités d'interface (le tactile travaille dans ces unités,
// pour que les boutons gardent la même taille relative sur tout écran).
void TouchUpdate(float ui_scale, float vw, float vh);

// Ajoute les commandes tactiles à celles déjà lues au clavier/souris.
// ads = 0..1 (en visée la visée tactile est ralentie, comme à la souris).
void TouchApply(PlayerInput *in, float ads);

void TouchDraw(void);    // joystick et boutons, en unités d'interface
bool TouchUsed(void);    // vrai si les commandes tactiles sont actives

#endif // TOUCH_H
