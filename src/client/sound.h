/*
 * sound.h — Sons synthétisés (aucun fichier audio dans le dépôt).
 *
 * Chaque son est GÉNÉRÉ au démarrage, en quelques millisecondes, à partir
 * de bruit, de sinusoïdes, d'enveloppes et d'un filtre passe-bas : tirs
 * (un timbre par famille d'arme), impacts, rechargement, pas, cris de
 * poussin, bip de la bombe, explosion, jingles de manche. Les sons du
 * monde sont atténués avec la distance à l'auditeur et répartis gauche /
 * droite selon la direction d'où ils viennent.
 *
 * Sans périphérique audio (machine sans carte son, émulateur en CI), tout
 * est silencieux et sans effet.
 */
#ifndef SOUND_H
#define SOUND_H

#include "raylib.h"

typedef enum {
    SND_SHOT_RIFLE = 0, // fusils d'assaut
    SND_SHOT_SMG,       // pistolet-mitrailleur
    SND_SHOT_PISTOL,
    SND_SHOT_SHOTGUN,
    SND_SHOT_SNIPER,
    SND_CLAW,           // griffe du poussin zombie
    SND_RELOAD,         // chargeur qui sort
    SND_SWITCH,         // arme armée, chargeur qui rentre
    SND_EMPTY,          // chargeur vide : clic sec
    SND_HITMARK,        // on a touché
    SND_HURT,           // on est touché
    SND_STEP,
    SND_LAND,
    SND_CHIRP,          // piaillement
    SND_BEEP,           // la bombe posée
    SND_PLANT,          // bombe posée
    SND_DEFUSE,         // bombe désamorcée
    SND_EXPLOSION,
    SND_ROUND_START,
    SND_WIN,
    SND_LOSE,
    SND_CLICK,          // interface
    SND_COUNT
} SoundId;

void SoundInit(void);                 // après InitWindow
void SoundClose(void);                // avant CloseWindow
void SoundSetVolume(float master);    // 0..1 (options)

// Où est l'auditeur (la caméra) : pour l'atténuation et la stéréo
void SoundSetListener(Vector3 pos, float yaw);

// Un son « dans la tête » (le sien, l'interface), à ce volume et cette
// hauteur (1 = normale)
void SoundPlay(int id, float volume, float pitch);

// Un son dans le monde, à cette position : atténué au-delà de `range` mètres
void SoundPlayAt(int id, Vector3 pos, float volume, float range);

int SoundForWeapon(int weapon);       // le son de tir d'une arme (WeaponId)

#endif // SOUND_H
