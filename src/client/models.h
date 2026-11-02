/*
 * models.h — Modèles 3D procéduraux des personnages (côté client).
 *
 * Les modèles ET leur outillage sont REPRIS DU PREMIER JEU (« ZEFFALIE
 * COME BACK », dépôt azerfjgpodvuirefefGB, src/render.c) : tout est
 * assemblé à partir de TROIS maillages seulement — un cube, une sphère,
 * un cylindre — posés par des matrices.
 *
 * Convention (celle du premier jeu) : MatrixMultiply(local, parent)
 * applique local PUIS parent. Un personnage est donc un arbre de
 * repères : pour dessiner le bec au bout de la tête, on passe la matrice
 * de la tête comme « parent » et la position du bec relative à la tête.
 * Quand la tête tourne, le bec suit tout seul — c'est ce qui permet de
 * vraies articulations (charnière d'aile, pivot du cou, mâchoire) au lieu
 * de pièces simplement déplacées.
 *
 * Repère local d'un personnage : origine au sol entre les pieds,
 * +Y vers le haut, +Z vers l'avant, unités en mètres.
 *
 * ModelsInit() doit être appelé APRÈS InitWindow (il crée les maillages)
 * et ModelsClose() AVANT CloseWindow.
 */
#ifndef MODELS_H
#define MODELS_H

#include <stdbool.h>

#include "raylib.h"
#include "raymath.h"

//----------------------------------------------------------------------------------
// Cycle de vie
//----------------------------------------------------------------------------------
void ModelsInit(void);    // crée le cube, la sphère et le cylindre (après InitWindow)
void ModelsClose(void);   // les libère (avant CloseWindow)

// Regroupement des primitives en UN SEUL maillage par image. Entre
// BatchBegin et BatchEnd, chaque Ball / Box3 / Seg n'est plus un appel de
// dessin : ses sommets sont transformés sur le CPU et ajoutés à un maillage
// dynamique, envoyé et dessiné une fois à BatchEnd. Tous les personnages et
// l'enseigne tiennent dans un appel au lieu d'un millier — c'est ce qui
// compte sur un téléphone, où chaque appel coûte cher. Hors d'un lot, les
// primitives se dessinent une à une comme avant.
void BatchBegin(void);
void BatchEnd(void);

//----------------------------------------------------------------------------------
// Primitives (mêmes signatures que render.h du premier jeu)
//----------------------------------------------------------------------------------
// t = translation, r = rotation (euler XYZ, radians), s = taille.
Matrix TRS(Vector3 t, Vector3 r, Vector3 s);
// Cube de taille s (côté 1 mis à l'échelle), centré en t, dans le repère parent
void Box3(Matrix parent, Vector3 t, Vector3 r, Vector3 s, Color c);
// Sphère (ou ellipsoïde aligné sur les axes) centrée en t, de rayons radii
void Ball(Matrix parent, Vector3 t, Vector3 radii, Color c);
// Ellipsoïde orienté : axes x et y orthonormés (z = x ^ y).
void BallAxes(Matrix parent, Vector3 t, Vector3 x, Vector3 y, Vector3 radii, Color c);
void Seg(Matrix parent, Vector3 a, Vector3 b, float radius, Color c);    // cylindre de a vers b
void Limb(Matrix parent, Vector3 a, Vector3 b, float radius, Color c);   // cylindre + rotules
void DrawBlobShadow(Vector3 p, float radius);   // ombre ronde au sol sous la position p
Color Shade(Color c, float k);                  // k < 1 assombrit, k > 1 éclaircit

//----------------------------------------------------------------------------------
// Animation
//----------------------------------------------------------------------------------
/*
 * Tout ce qui bouge sur un personnage est décrit ici. L'appelant garde un
 * CharAnim PAR JOUEUR, l'avance à chaque image avec CharAnimUpdate (qui
 * déduit la vitesse du déplacement affiché) et déclenche les actions
 * ponctuelles quand le réseau les annonce (CharAnimAttack sur un coup
 * porté, CharAnimHit sur une balle reçue).
 *
 * Les champs qui descendent de 1 vers 0 sont des comptes à rebours en
 * secondes normalisées : 1 = l'action vient de commencer, 0 = terminée.
 */
typedef struct {
    // --- suivi du déplacement ---
    float t;            // horloge propre au personnage (décalée : tics désynchronisés)
    float phase;        // phase du pas, avance avec la DISTANCE parcourue
    float move;         // 0..1 : à quel point il se déplace (amplitude du balancement)
    float run;          // 0..1 : 0 = marche, 1 = course (foulée longue, corps penché)
    float air;          // 0..1 : en l'air (pattes repliées, ailes qui battent)
    float land;         // 1 -> 0 : amorti d'atterrissage (pattes qui plient)
    float squash;       // 0..1 : écrasement vertical au moment où un pied se pose

    // --- actions ponctuelles ---
    float attack;       // 1 -> 0 : bond de griffe, bec qui claque
    float flinch;       // 1 -> 0 : encaisse une balle (sursaut en arrière)
    float death;        // 0 -> 1 : s'effondre, puis reste couché
    float dead_roll;    // -1 ou +1 : côté vers lequel il tombe (tiré au sort)

    // --- tics et réflexes (jamais synchronisés d'un personnage à l'autre) ---
    float blink;        // 1 -> 0 : paupière qui se ferme
    float blink_in;     // secondes avant le prochain clignement
    float shake;        // 1 -> 0 : ébrouement (il secoue ses plumes)
    float shake_in;     // secondes avant le prochain ébrouement
    float gape;         // 0..1 : ouverture du bec (lissée)
    float drool;        // 0..1 : la bave s'allonge puis tombe

    // --- regard ---
    float look;         // -1..1 : rotation de la tête vers sa proie (lissée)
    float look_goal;    // cible de "look", posée par CharAnimLookAt
} CharAnim;

// seed décale l'horloge et les tics : deux poussins ne clignent pas des
// yeux ensemble et ne s'ébrouent pas en même temps.
void CharAnimInit(CharAnim *a, int seed);

// Avance l'animation d'une image. from / to = position affichée avant et
// après le lissage de cette image : la vitesse horizontale (marche/course)
// et verticale (saut/chute) en sont déduites, l'appelant n'a rien à
// calculer. alive = le serveur le donne encore vivant (false lance
// l'effondrement, true l'annule).
void CharAnimUpdate(CharAnim *a, float dt, Vector3 from, Vector3 to, bool alive);

void CharAnimAttack(CharAnim *a);    // il vient de griffer / donner un coup de bec
void CharAnimHit(CharAnim *a);       // il vient d'encaisser une balle
void CharAnimTeleport(CharAnim *a);  // il a réapparu ailleurs : efface la mort

// Oriente la tête vers un point du monde. yaw = orientation du corps ;
// has_target = false laisse la tête revenir doucement dans l'axe.
void CharAnimLookAt(CharAnim *a, Vector3 self, float yaw, Vector3 target, bool has_target);

//----------------------------------------------------------------------------------
// Les modèles
//----------------------------------------------------------------------------------
// Le soldat-mercenaire : les JOUEURS humains (treillis olive, visage peint
// en vert, démarche de zombie bras tendus).
void DrawSoldier(Vector3 pos, float yaw, const CharAnim *a, Color team_color);

// Le poussin zombie : les BOTS (poussin humanoïde d'1m60 ensanglanté).
void DrawZombieChick(Vector3 pos, float yaw, const CharAnim *a, Color team_color);

// Le petit poussin du premier jeu (environ 25 cm), porté tel quel avec sa
// signature d'origine pour rester interchangeable avec azerf…/src/render.c.
// t = temps d'animation (décaler pour désynchroniser deux poussins),
// flap = battements d'ailes forcés (0..1).
void DrawChick(Matrix parent, float t, float flap);

#endif // MODELS_H
