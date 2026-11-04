/*
 * weapons.h — Modèles 3D procéduraux des armes (côté client).
 *
 * Comme les personnages (models.h), chaque arme est assemblée à partir des
 * trois maillages de base (cube, sphère, cylindre) posés par des matrices,
 * et passe donc par le lot (batch) : la vue à la première personne et les
 * armes tenues par les autres joueurs coûtent un seul appel de dessin.
 *
 * Repère d'une arme : origine au boîtier (au-dessus de la poignée),
 * canon vers -Z, +Y vers le haut, +X vers la droite, unités en mètres.
 * La LIGNE DE VISÉE de chaque arme (hausse, point rouge ou lunette) est
 * à y = WEAPON_SIGHT_Y : en visée, le client place cette ligne au centre
 * de l'écran, quelle que soit l'arme.
 *
 * Le même modèle sert en vue subjective (parent = repère de l'arme en
 * espace vue) et dans les mains des autres soldats (parent = repère de
 * leur torse) : WeaponGrips donne où poser leurs mains.
 */
#ifndef WEAPONS_H
#define WEAPONS_H

#include <stdbool.h>
#include "raylib.h"
#include "raymath.h"

#define WEAPON_SIGHT_Y 0.085f

// Ce qui bouge sur une arme
typedef struct {
    float muzzle;     // 0..1 : flash de bouche (taille)
    float mag_out;    // 0..1 : chargeur sorti (rechargement)
    float bolt;       // 0..1 : culasse / pompe reculée (réarmement)
} WeaponPose;

// Dessine l'arme `weapon` (WeaponId de protocol.h) dans le repère parent.
void DrawWeaponModel(Matrix parent, int weapon, const WeaponPose *pose);

// Bout du canon, dans le repère de l'arme (départ des traçantes)
Vector3 WeaponMuzzle(int weapon);

// Où les mains se posent : poignée (main droite) et garde-main (main gauche)
void WeaponGrips(int weapon, Vector3 *right, Vector3 *left);

// Un fusil à lunette ? (la visée passe alors par une lunette plein écran)
bool WeaponHasScope(int weapon);

#endif // WEAPONS_H
