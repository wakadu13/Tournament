/*
 * options.h — Réglages du joueur, gardés d'une partie à l'autre.
 *
 * Tout ce que le menu « Options » permet de changer, plus ce que le jeu
 * retient de lui-même (dernier serveur, pseudo, équipement choisi). Le
 * tout est écrit dans un petit fichier texte « clé=valeur » :
 *   - sur PC, à côté de l'exécutable (GetApplicationDirectory) ;
 *   - sur Android, dans le dossier privé de l'application (raylib y
 *     redirige SaveFileText / LoadFileText).
 * Un fichier absent ou abîmé donne simplement les valeurs par défaut.
 */
#ifndef OPTIONS_H
#define OPTIONS_H

#include <stdbool.h>

typedef struct {
    float sensitivity;     // souris : multiplicateur, 0,2 .. 3
    float touch_sens;      // visée au doigt : multiplicateur, 0,2 .. 3
    float fov;             // champ de vision à la hanche, en degrés, 60 .. 100
    bool  invert_y;        // souris / doigt inversés sur l'axe vertical
    bool  fullscreen;      // PC : plein écran
    bool  show_fps;
    bool  auto_reload;     // recharge seul quand le chargeur est vide
    float volume;          // volume général, 0 .. 1
    int   primary;         // WeaponId de l'arme principale choisie
    int   secondary;       // WeaponId de l'arme de poing choisie
    char  name[16];        // pseudo (NAME_MAX)
    char  server[64];      // dernière adresse de serveur
} Options;

extern Options options;    // les réglages en vigueur

void OptionsDefaults(void);   // remet les valeurs par défaut
void OptionsLoad(void);       // lit le fichier (sinon : défauts)
bool OptionsSave(void);       // écrit le fichier ; false si impossible

#endif // OPTIONS_H
