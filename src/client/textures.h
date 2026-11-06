/*
 * textures.h — Textures procédurales de la carte.
 *
 * Rien n'est chargé depuis un fichier : sable, pisé, bois, pierre, tôle,
 * toile et tuiles sont GÉNÉRÉS au démarrage (bruit de valeur à plusieurs
 * octaves, rayures, joints, fissures), en 256 x 256, avec mipmaps. Chaque
 * texture est confiée à un matériau du lot de rendu (BatchSetTexture),
 * qui la projette sur les faces des boîtes de la carte.
 *
 * TexturesInit() après ModelsInit() ; TexturesClose() avant CloseWindow.
 */
#ifndef TEXTURES_H
#define TEXTURES_H

void TexturesInit(void);
void TexturesClose(void);

#endif // TEXTURES_H
