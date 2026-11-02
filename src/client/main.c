/*
 * client/main.c — Client FPS raylib.
 *
 * Usage : ./client [ip_serveur] [pseudo]
 *
 * Principe général :
 *   - Le client simule SON PROPRE déplacement localement ("prédiction") :
 *     le jeu reste fluide quel que soit le ping, et il envoie sa position
 *     au serveur CLIENT_SEND_HZ fois par seconde.
 *   - Les AUTRES joueurs ne sont pas simulés : on glisse en douceur vers
 *     leur dernière position reçue dans les snapshots (interpolation).
 *   - Le serveur reste autoritaire sur tout ce qui compte : les tirs
 *     (raycast côté serveur), la vie, les kills, les scores, les respawns.
 *
 * Vue FPS (inspirée du projet "ZeffalieComeBack") :
 *   - Caméra aux yeux du joueur, avec balancement de marche (head bob),
 *     recul de l'arme qui fait monter le canon, et FOV qui change selon
 *     la visée (zoom) ou le sprint (impression de vitesse).
 *   - L'arme ("view model") est dessinée dans une passe 3D séparée, avec
 *     une caméra fixe à l'origine : elle est décrite en "espace vue"
 *     (x = droite, y = haut, -z = devant la caméra) et ne rentre jamais
 *     dans les murs. Elle balance à la marche, "traîne" derrière les
 *     mouvements de souris (sway) et recule à chaque tir (kick).
 *   - La dispersion des balles dépend de la situation : précise en visant,
 *     imprécise en courant, en sautant ou en tirant en rafale (bloom).
 *     Le réticule s'écarte à l'écran pour la montrer.
 */

// platform.h EN PREMIER : sous Windows, winsock2.h tire windows.h, qui
// declare Rectangle, CloseWindow, ShowCursor et DrawText — les memes noms
// que raylib. platform.h ecarte les en-tetes fautifs, mais seulement si
// ses macros sont posees AVANT raylib.h.
#include "../common/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "raylib.h"
#include "raymath.h"   // matrices et vecteurs (viewmodel de l'arme)
#include "rlgl.h"      // accès au pipeline (pile de matrices, depth test)

#include "../common/protocol.h"
#include "../common/game.h"
#include "models.h"   // modèles 3D des personnages (soldat / poussin zombie)
#include "touch.h"    // commandes tactiles (mobile) et structure PlayerInput

//----------------------------------------------------------------------------------
// Réglages de la vue FPS
//----------------------------------------------------------------------------------
#define SCREEN_W 1280
#define SCREEN_H 720

#define MOUSE_SENSITIVITY 0.0025f  // radians par pixel de souris

// Interface mobile : écran de connexion au doigt et commandes tactiles.
// Même interrupteur que TOUCH_ENABLED dans touch.c, pour que les deux
// fichiers soient toujours d'accord. -DTOUCH_CONTROLS=1 permet de la
// mettre au point sur PC, sans téléphone.
#if defined(PLATFORM_ANDROID) || defined(TOUCH_CONTROLS)
    #define MOBILE_UI 1
#else
    #define MOBILE_UI 0
#endif

//----------------------------------------------------------------------------------
// Échelle de l'interface
//----------------------------------------------------------------------------------
// Le HUD est dessiné en UNITÉS D'INTERFACE : 720 de haut, largeur selon le
// ratio de l'écran. Sur PC la fenêtre fait exactement SCREEN_W x SCREEN_H et
// une unité vaut un pixel. Sur Android la fenêtre a la taille native de
// l'écran (InitWindow(0, 0)), bien plus grand et souvent en 20:9 : l'échelle
// suit la hauteur, la largeur virtuelle suit le ratio, et BeginMode2D met
// tout à l'échelle d'un coup. Les boutons gardent ainsi la même taille
// relative d'un téléphone à une tablette, sans bande noire.
#define UI_H 720                      // hauteur de référence
static float ui_scale = 1.0f;         // pixels réels par unité d'interface
static float ui_w = (float)SCREEN_W;  // largeur de l'écran EN UNITÉS
static float ui_h = (float)UI_H;
#define UIW ((int)ui_w)
#define UIH ((int)ui_h)

// À appeler une fois par image : l'écran d'un mobile peut tourner.
static void UiUpdate(void)
{
    int rw = GetScreenWidth(), rh = GetScreenHeight();
    if (rw < 1 || rh < 1) { rw = SCREEN_W; rh = SCREEN_H; }
    ui_scale = (float)rh / (float)UI_H;   // on cale sur la hauteur
    ui_h = (float)UI_H;
    ui_w = (float)rw / ui_scale;          // la largeur suit le ratio de l'écran
}

// Caméra 2D qui applique cette échelle à tout le dessin de l'interface
static Camera2D UiCamera(void)
{
    Camera2D c = {0};
    c.zoom = ui_scale;
    return c;
}

// Champ de vision (degrés) selon la situation. La valeur réelle rejoint
// la cible en douceur à chaque frame (pas de saut brutal).
#define FOV_HIP    75.0f           // normal, arme à la hanche
#define FOV_ADS    50.0f           // en visant : effet de zoom
#define FOV_SPRINT 84.0f           // en sprint : impression de vitesse

#define SPRINT_SPEED      10.5f    // m/s en sprint (PLAYER_SPEED sinon)
#define ACCEL_GROUND      12.0f    // accélération au sol (réactif)
#define ACCEL_AIR         2.5f     // en l'air : faible contrôle aérien

#define INTERP_SPEED      15.0f    // vitesse de lissage des autres joueurs

#define KILLFEED_MAX   5
#define KILLFEED_TTL   5.0f        // durée d'affichage d'une ligne (s)
#define TRACER_TTL     0.06f       // durée de vie d'une traçante (s)
#define TRACER_MAX     16

//----------------------------------------------------------------------------------
// État réseau
//----------------------------------------------------------------------------------
static socket_t           sock_fd = SOCKET_NONE;   // socket UDP (non bloquant)
static struct sockaddr_in server_addr;      // adresse du serveur
static int                my_id = -1;       // notre numéro de slot côté serveur
static Team               my_team = TEAM_RED;
static double             last_server_msg = 0.0;  // pour détecter une coupure

// Dernier snapshot reçu (état du monde vu par le serveur) et, pour chaque
// joueur, la position réellement AFFICHÉE (lissée entre deux snapshots).
static PacketSnapshot snap;
static Vector3        display_pos[MAX_PLAYERS];
static bool           has_display[MAX_PLAYERS];

// Animation des autres joueurs. Tout l'état d'un personnage (marche,
// course, saut, atterrissage, tics, griffade, sursaut, effondrement) tient
// dans un CharAnim par joueur, que CharAnimUpdate avance à partir du seul
// déplacement affiché : voir models.h.
static CharAnim anim[MAX_PLAYERS];

//----------------------------------------------------------------------------------
// État local du joueur (prédiction)
//----------------------------------------------------------------------------------
static V3    my_pos;               // position des PIEDS (comme sur le serveur)
static V3    my_vel = {0};         // vitesse horizontale actuelle (m/s)
static float my_vel_y = 0.0f;      // vitesse verticale (saut / gravité)
static float my_yaw = 0.0f;        // regard gauche/droite (radians)
static float my_pitch = 0.0f;      // regard haut/bas (radians)
static bool  my_alive = true;      // décidé par le serveur
static uint8_t my_spawn_count = 0; // compteur de respawn (voir protocol.h)
static bool  on_ground = true;
static bool  sprinting = false;
static float dead_t = 0.0f;        // temps écoulé depuis notre mort (anim caméra)
static int   last_health = PLAYER_MAX_HEALTH;

//----------------------------------------------------------------------------------
// État de l'arme et des animations de la vue
// Toutes ces valeurs sont purement visuelles SAUF recoil_* et la dispersion,
// qui modifient réellement la direction des balles envoyée au serveur.
//----------------------------------------------------------------------------------
static float ads = 0.0f;           // 0 = hanche, 1 = en visée (transition douce)
static float sprint_t = 0.0f;      // 0..1 : arme baissée pendant le sprint
static float kick = 0.0f;          // 1 juste après un tir, retombe vite (recul visuel)
static float recoil_pitch = 0.0f;  // le canon monte à chaque tir...
static float recoil_yaw = 0.0f;    // ...et dévie un peu au hasard
static float bloom = 0.0f;         // imprécision accumulée en rafale
static float muzzle_t = 0.0f;      // flash de bouche encore visible (s)
static float bob = 0.0f;           // phase du balancement de marche
static Vector2 sway = {0};         // l'arme "traîne" derrière la souris
static float shoot_cooldown = 0.0f;
static float hit_marker_timer = 0.0f;    // réticule rouge quand on touche
static float damage_flash_timer = 0.0f;  // voile rouge quand on est touché

//----------------------------------------------------------------------------------
// Effets : fil des kills et traçantes
//----------------------------------------------------------------------------------
typedef struct {
    char  text[64];
    float ttl;                     // temps restant à l'écran
} KillfeedEntry;
static KillfeedEntry killfeed[KILLFEED_MAX];

typedef struct {
    Vector3 from, to;              // du canon au point d'impact
    float   ttl;
    bool    active;
} Tracer;
static Tracer tracers[TRACER_MAX];

// Couleur d'une équipe (HUD, corps des joueurs, marquages au sol)
static Color TeamColor(int team)
{
    return (team == TEAM_RED) ? (Color){230, 60, 50, 255}
                              : (Color){50, 120, 230, 255};
}

// Nombre aléatoire dans [a, b] (dispersion, recul)
static float Randf(float a, float b)
{
    return a + (b - a) * ((float)GetRandomValue(0, 10000) / 10000.0f);
}

// Ajoute une ligne en haut du killfeed (les autres descendent d'un cran)
static void KillfeedPush(const char *text)
{
    for (int i = KILLFEED_MAX - 1; i > 0; i--) killfeed[i] = killfeed[i - 1];
    snprintf(killfeed[0].text, sizeof(killfeed[0].text), "%s", text);
    killfeed[0].ttl = KILLFEED_TTL;
}

static void TracerAdd(Vector3 from, Vector3 to)
{
    for (int i = 0; i < TRACER_MAX; i++) {
        if (!tracers[i].active) {
            tracers[i] = (Tracer){from, to, TRACER_TTL, true};
            return;
        }
    }
}

//----------------------------------------------------------------------------------
// Réseau
//----------------------------------------------------------------------------------
// Ouvre le socket et fait la "poignée de main" : on envoie PKT_CL_CONNECT
// jusqu'à recevoir PKT_SV_ACCEPT (l'UDP peut perdre des paquets, donc on
// réessaie toutes les 0,5 s, 10 fois maximum).
static bool NetConnect(const char *ip, const char *name)
{
    if (!PlatformNetInit()) return false;
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (SocketFailed(sock_fd)) return false;
    SocketSetNonBlocking(sock_fd);   // recvfrom ne bloquera jamais

    // Accepte une IP comme un NOM DNS, et un port colle a l'hote
    // (« jeu.exemple.fr:27015 ») ; sans port, DEFAULT_PORT s'applique.
    if (!ResolveServerAddr(ip, DEFAULT_PORT, &server_addr)) {
        printf("Serveur introuvable : %s\n", ip);
        return false;
    }

    PacketConnect pkt = {0};
    pkt.type = PKT_CL_CONNECT;
    pkt.version = PROTO_VERSION;
    snprintf(pkt.name, NAME_MAX, "%s", name);

    for (int attempt = 0; attempt < 10; attempt++) {
        sendto(sock_fd, &pkt, sizeof(pkt), 0,
               (struct sockaddr *)&server_addr, sizeof(server_addr));

        // On écoute la réponse pendant une demi-seconde
        double deadline = GetTime() + 0.5;
        while (GetTime() < deadline) {
            uint8_t buf[2048];
            sock_ssize_t n = recvfrom(sock_fd, buf, sizeof(buf), 0, NULL, NULL);
            if (n >= (sock_ssize_t)sizeof(PacketAccept) && buf[0] == PKT_SV_ACCEPT) {
                PacketAccept *acc = (PacketAccept *)buf;
                my_id = acc->player_id;       // le serveur nous donne notre slot
                my_team = (Team)acc->team;    // et notre équipe (auto-équilibrage)
                last_server_msg = GetTime();
                printf("Connecté : id %d, équipe %s\n", my_id,
                       my_team == TEAM_RED ? "ROUGE" : "BLEUE");
                return true;
            }
            if (n >= (sock_ssize_t)sizeof(PacketHeader) && buf[0] == PKT_SV_REJECT) {
                PacketReject *rej = (PacketReject *)buf;
                printf("Connexion refusée : %.32s\n", rej->reason);
                return false;
            }
            PlatformSleepMs(10);
        }
        printf("Connexion au serveur %s... (%d/10)\n", ip, attempt + 1);
    }
    printf("Serveur injoignable.\n");
    return false;
}

// Envoie notre position/orientation (appelé CLIENT_SEND_HZ fois par seconde)
static void NetSendState(void)
{
    PacketClientState pkt = {
        PKT_CL_STATE, (uint8_t)my_id, my_spawn_count,
        my_pos.x, my_pos.y, my_pos.z,
        my_yaw, my_pitch,
    };
    sendto(sock_fd, &pkt, sizeof(pkt), 0,
           (struct sockaddr *)&server_addr, sizeof(server_addr));
}

// Envoie un tir : le rayon (origine + direction) part de nos yeux.
// C'est le SERVEUR qui décide si quelqu'un est touché, pas nous.
static void NetSendShoot(V3 origin, V3 dir)
{
    PacketShoot pkt = {
        PKT_CL_SHOOT, (uint8_t)my_id,
        origin.x, origin.y, origin.z,
        dir.x, dir.y, dir.z,
    };
    sendto(sock_fd, &pkt, sizeof(pkt), 0,
           (struct sockaddr *)&server_addr, sizeof(server_addr));
}

// Départ propre (sinon le serveur nous enlèvera au timeout de 5 s)
static void NetSendDisconnect(void)
{
    PacketDisconnect pkt = {PKT_CL_DISCONNECT, (uint8_t)my_id};
    sendto(sock_fd, &pkt, sizeof(pkt), 0,
           (struct sockaddr *)&server_addr, sizeof(server_addr));
}

static const char *PlayerName(int id)
{
    if (id < 0 || id >= MAX_PLAYERS || !snap.players[id].active) return "???";
    return snap.players[id].name;
}

// Réaction aux événements ponctuels envoyés par le serveur
static void HandleEvent(const PacketEvent *ev)
{
    char line[64];
    switch (ev->event) {
    case EV_KILL:
        snprintf(line, sizeof(line), "%s a éliminé %s",
                 PlayerName(ev->a), PlayerName(ev->b));
        KillfeedPush(line);
        if (ev->a == my_id) hit_marker_timer = 0.3f;   // c'est nous le tueur
        // Le tueur porte son coup. La victime, elle, s'effondre dès que le
        // snapshot la donne pour morte (CharAnimUpdate s'en charge).
        if (ev->a < MAX_PLAYERS) CharAnimAttack(&anim[ev->a]);
        break;
    case EV_HIT:
        if (ev->a == my_id) hit_marker_timer = 0.15f;  // on a touché quelqu'un
        if (ev->b == my_id) damage_flash_timer = 0.25f; // on a été touché
        // Celui qui frappe porte son coup (pour un bot : le bond de griffe,
        // bec qui claque) et celui qui encaisse sursaute.
        if (ev->a < MAX_PLAYERS) CharAnimAttack(&anim[ev->a]);
        if (ev->b < MAX_PLAYERS) CharAnimHit(&anim[ev->b]);
        break;
    case EV_RESPAWN:
        // Il réapparaît ailleurs : on efface l'effondrement d'un coup,
        // sinon son cadavre se relèverait avant de se téléporter.
        if (ev->a < MAX_PLAYERS) CharAnimTeleport(&anim[ev->a]);
        break;
    case EV_PLAYER_JOIN:
        snprintf(line, sizeof(line), "%s a rejoint la partie", PlayerName(ev->a));
        KillfeedPush(line);
        break;
    case EV_PLAYER_LEAVE:
        KillfeedPush("un joueur est parti");
        break;
    default:
        break;
    }
}

// Lit TOUS les paquets en attente sur le socket (il peut y en avoir
// plusieurs par frame : snapshots + événements)
static void NetReceive(void)
{
    for (;;) {
        uint8_t buf[4096];
        sock_ssize_t n = recvfrom(sock_fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n <= 0) break;         // plus rien à lire
        last_server_msg = GetTime();

        if (buf[0] == PKT_SV_SNAPSHOT && n >= (sock_ssize_t)sizeof(PacketSnapshot)) {
            memcpy(&snap, buf, sizeof(snap));

            // Le serveur fait autorité sur notre vie. Quand son compteur
            // de respawn change (y compris au tout premier spawn), on
            // reprend la position qu'IL a choisie, et on écho le nouveau
            // compteur dans nos paquets d'état.
            SnapshotPlayer *me = &snap.players[my_id];
            if (me->active) {
                if (me->spawn_count != my_spawn_count) {   // (re)spawn
                    my_spawn_count = me->spawn_count;
                    my_pos = (V3){me->x, me->y, me->z};
                    my_yaw = me->yaw;
                    my_pitch = me->pitch;
                    my_vel = (V3){0};
                    my_vel_y = 0.0f;
                    dead_t = 0.0f;
                }
                my_alive = me->alive != 0;
                // La vie a baissé depuis le dernier snapshot : voile rouge
                if (me->health < last_health) damage_flash_timer = 0.25f;
                last_health = me->health;
            }
        } else if (buf[0] == PKT_SV_EVENT && n >= (sock_ssize_t)sizeof(PacketEvent)) {
            HandleEvent((const PacketEvent *)buf);
        }
    }
}

//----------------------------------------------------------------------------------
// Vue et tir
//----------------------------------------------------------------------------------
// Direction du regard (vecteur unitaire), RECUL COMPRIS : c'est donc aussi
// la direction où partent les balles (avant dispersion). Conversion
// angles -> vecteur : yaw = 0 regarde vers +Z, pitch > 0 vers le haut.
static V3 LookDir(void)
{
    float pitch = my_pitch + recoil_pitch;
    float yaw = my_yaw + recoil_yaw;
    return (V3){
        cosf(pitch) * sinf(yaw),
        sinf(pitch),
        cosf(pitch) * cosf(yaw),
    };
}

// Position des yeux (la caméra), au-dessus des pieds
static V3 EyePos(void)
{
    return (V3){my_pos.x, my_pos.y + PLAYER_EYE_HEIGHT, my_pos.z};
}

// Dispersion des balles : rayon du cône de tir (~radians). Dépend de :
//   - la visée : 0.022 à la hanche -> 0.002 en visant,
//   - la vitesse de déplacement (courir rend imprécis),
//   - le saut (+0.04 en l'air),
//   - le bloom, qui grossit à chaque tir et redescend avec le temps.
// Le réticule du HUD s'écarte avec cette même valeur.
static float WeaponSpread(void)
{
    float hspeed = sqrtf(my_vel.x * my_vel.x + my_vel.z * my_vel.z);
    return Lerp(0.022f, 0.002f, ads)
         + hspeed * 0.003f
         + (on_ground ? 0.0f : 0.04f)
         + bloom;
}

// Position de l'arme par rapport à la caméra, en ESPACE VUE
// (x = droite, y = haut, -z = devant). On mélange :
//   - la position hanche/visée selon "ads",
//   - le balancement de marche (bob) : petit "8" horizontal + rebond,
//   - le sway (l'arme traîne derrière la souris),
//   - le recul (kick) qui ramène l'arme vers l'épaule,
//   - l'arme baissée pendant le sprint.
static Vector3 WeaponOffset(void)
{
    Vector3 hip = {0.17f, -0.19f, -0.45f};   // en bas à droite de l'écran
    Vector3 aim = {0.0f, -0.085f, -0.33f};   // viseur au centre de l'écran
    Vector3 o = Vector3Lerp(hip, aim, ads);

    float bob_amount = 1.0f - ads * 0.85f;   // presque pas de bob en visée
    o.x += sinf(bob) * 0.012f * bob_amount + sway.x;
    o.y += -fabsf(cosf(bob)) * 0.012f * bob_amount + sway.y;
    o.z += kick * 0.06f;                     // recul vers l'épaule
    o.y -= sprint_t * 0.06f;                 // baissée en sprint
    o.x -= sprint_t * 0.04f;
    return o;
}

// Repère complet de l'arme en espace vue : rotation (recul qui fait
// basculer le canon, inclinaison de sprint) puis translation (WeaponOffset)
static Matrix WeaponViewMatrix(void)
{
    Vector3 o = WeaponOffset();
    Vector3 rot = {
        kick * 0.12f - sprint_t * 0.25f,     // X : canon qui bascule
        sprint_t * 0.7f,                     // Y : arme tournée vers la gauche
        sprint_t * 0.35f,                    // Z : roulis (penchée)
    };
    return MatrixMultiply(MatrixRotateXYZ(rot), MatrixTranslate(o.x, o.y, o.z));
}

// Convertit un point de l'espace vue vers le monde : on reconstruit la base
// de la caméra (f = devant, r = droite, u = haut) et on s'en sert comme
// axes. En espace vue -z est devant, d'où le "f * -v.z". Sert à faire
// partir les traçantes du bout du canon VISIBLE à l'écran.
static Vector3 ViewToWorld(Vector3 v)
{
    V3 fv = LookDir();
    Vector3 f = {fv.x, fv.y, fv.z};
    Vector3 r = Vector3Normalize(Vector3CrossProduct(f, (Vector3){0, 1, 0}));
    Vector3 u = Vector3CrossProduct(r, f);
    V3 eye = EyePos();
    return (Vector3){
        eye.x + r.x * v.x + u.x * v.y + f.x * -v.z,
        eye.y + r.y * v.x + u.y * v.y + f.y * -v.z,
        eye.z + r.z * v.x + u.z * v.y + f.z * -v.z,
    };
}

// Bout du canon en espace vue (le -0.62 correspond au modèle de l'arme)
static Vector3 MuzzleView(void)
{
    return Vector3Transform((Vector3){0, 0.02f, -0.62f}, WeaponViewMatrix());
}

// Tire une balle : recul, flash, dispersion, envoi au serveur et traçante.
static void Shoot(void)
{
    shoot_cooldown = WEAPON_COOLDOWN;
    muzzle_t = 0.05f;              // flash de bouche pendant 50 ms
    kick = 1.0f;                   // animation de recul de l'arme
    // Le canon monte (moins en visant) et dévie un peu au hasard. Il
    // redescend tout seul (décroissance dans UpdateViewEffects).
    recoil_pitch += Randf(0.010f, 0.016f) * (1.0f - ads * 0.35f);
    recoil_yaw += Randf(-0.006f, 0.006f);
    bloom = fminf(bloom + 0.006f, 0.04f);   // rafale = moins précis (plafonné)

    // Direction du tir = regard (recul compris) + dispersion aléatoire dans
    // un cône : on décale la direction dans le plan perpendiculaire (r, u).
    V3 fv = LookDir();
    Vector3 f = {fv.x, fv.y, fv.z};
    Vector3 r = Vector3Normalize(Vector3CrossProduct(f, (Vector3){0, 1, 0}));
    Vector3 u = Vector3CrossProduct(r, f);
    float ang = Randf(0, 2 * PI);              // angle autour de l'axe
    float mag = Randf(0, 1) * WeaponSpread();  // écart (0..spread)
    Vector3 d = Vector3Normalize((Vector3){
        f.x + (r.x * cosf(ang) + u.x * sinf(ang)) * mag,
        f.y + (r.y * cosf(ang) + u.y * sinf(ang)) * mag,
        f.z + (r.z * cosf(ang) + u.z * sinf(ang)) * mag,
    });

    V3 eye = EyePos();
    V3 dir = {d.x, d.y, d.z};
    NetSendShoot(eye, dir);        // le serveur décide des dégâts

    // Traçante locale (purement visuelle) : du canon visible jusqu'au
    // premier obstacle (mur ou joueur affiché).
    float t = RaycastMap(eye, dir);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        SnapshotPlayer *sp = &snap.players[i];
        if (!sp->active || !sp->alive || i == my_id) continue;
        float ht;
        V3 ppos = {display_pos[i].x, display_pos[i].y, display_pos[i].z};
        if (RayVsBox(eye, dir, PlayerBox(ppos), &ht) && ht > 0 && ht < t) t = ht;
    }
    Vector3 to = {eye.x + dir.x * t, eye.y + dir.y * t, eye.z + dir.z * t};
    TracerAdd(ViewToWorld(MuzzleView()), to);
}

//----------------------------------------------------------------------------------
// Mise à jour du joueur local
//----------------------------------------------------------------------------------
static void UpdateLocalPlayer(float dt)
{
    // Les commandes sont rassemblées ici, d'où qu'elles viennent : clavier
    // et souris sur PC, doigts sur mobile. Le reste de la fonction ne sait
    // plus sur quelle machine il tourne.
    PlayerInput in = {0};

    // --- Souris : rotation de la vue (moins sensible en visant) ---
    // ATTENTION : sur mobile, raylib fait passer le tactile POUR la souris.
    // Lire la souris ici ajouterait le glissement du doigt une deuxieme
    // fois (visee deux fois trop rapide, et le joystick ferait tourner la
    // vue) et declencherait un tir des qu'un doigt se pose, n'importe ou.
    // On ne la lit donc que sur PC : sur mobile, visee et boutons viennent
    // uniquement de touch.c.
#if !MOBILE_UI
    float sens = MOUSE_SENSITIVITY * (1.0f - ads * 0.35f);
    Vector2 md = GetMouseDelta();
    in.look.x = -md.x * sens;
    in.look.y = -md.y * sens;
    in.fire = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    in.aim  = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
#endif

    // --- Clavier : direction voulue. KEY_* correspond à la POSITION des
    // touches (comme un QWERTY) : WASD marche donc en ZQSD sur un clavier
    // AZERTY. On accepte aussi Z/Q au cas où le clavier serait mappé.
    // Sans effet sur un telephone nu, et utile avec un clavier Bluetooth. ---
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_Z)) in.move.y += 1;   // avancer
    if (IsKeyDown(KEY_S)) in.move.y -= 1;                       // reculer
    if (IsKeyDown(KEY_D)) in.move.x += 1;                       // droite
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_Q)) in.move.x -= 1;   // gauche
    if (IsKeyDown(KEY_SPACE)) in.jump = true;
    if (IsKeyDown(KEY_LEFT_SHIFT)) in.sprint = true;

    // --- Doigts : s'ajoutent aux commandes ci-dessus (sans effet sur PC) ---
    TouchApply(&in, ads);

    // En diagonale la longueur vaudrait ~1.41 : on la ramène à 1 pour ne
    // pas se déplacer plus vite en biais.
    if (Vector2Length(in.move) > 1.0f) in.move = Vector2Normalize(in.move);

    my_yaw += in.look.x;
    my_pitch += in.look.y;
    // Pitch limité à ~±86° : impossible de "passer par-dessus" la verticale
    my_pitch = Clamp(my_pitch, -1.5f, 1.5f);

    Vector2 move = in.move;
    bool firing = in.fire;
    bool aiming = in.aim;
    // Sprint : en avançant, incompatible avec viser/tirer
    sprinting = in.sprint && move.y > 0.3f && !aiming && !firing;

    // --- Déplacement : vitesse voulue puis accélération progressive.
    // "devant" et "droite" sont calculés à plat (sans le pitch) : on se
    // déplace au sol même en regardant en l'air. ---
    V3 fwd = {sinf(my_yaw), 0, cosf(my_yaw)};
    V3 right = {fwd.z, 0, -fwd.x};
    float speed = (sprinting ? SPRINT_SPEED : PLAYER_SPEED)
                * (1.0f - ads * 0.4f);   // -40 % en visant
    V3 wish = {
        (fwd.x * move.y + right.x * move.x) * speed, 0,
        (fwd.z * move.y + right.z * move.x) * speed,
    };
    // La vitesse actuelle se rapproche de la vitesse voulue d'une fraction
    // (dt * accel) par frame : démarrage et freinage en douceur, et faible
    // contrôle en l'air (on garde son élan pendant un saut).
    float accel = on_ground ? ACCEL_GROUND : ACCEL_AIR;
    my_vel.x += (wish.x - my_vel.x) * fminf(1.0f, dt * accel);
    my_vel.z += (wish.z - my_vel.z) * fminf(1.0f, dt * accel);

    // Saut : seulement au sol
    if (in.jump && on_ground) my_vel_y = PLAYER_JUMP_SPEED;

    // Collisions et gravité (code partagé avec le serveur/bot)
    MovePlayer(&my_pos, my_vel, &my_vel_y, dt, &on_ground);

    // Phase du balancement de marche : avance avec la vitesse au sol
    float hspeed = sqrtf(my_vel.x * my_vel.x + my_vel.z * my_vel.z);
    if (on_ground) bob += dt * hspeed * 1.6f;

    // --- Tir : automatique tant que le bouton est maintenu, à la cadence
    // WEAPON_COOLDOWN, et jamais pendant un sprint ---
    if (shoot_cooldown > 0.0f) shoot_cooldown -= dt;
    if (firing && !sprinting && shoot_cooldown <= 0.0f) Shoot();

    // --- Transitions douces de la visée et du sprint (0 -> 1) ---
    ads += ((aiming && !sprinting ? 1.0f : 0.0f) - ads) * fminf(1.0f, dt * 14.0f);
    sprint_t += ((sprinting ? 1.0f : 0.0f) - sprint_t) * fminf(1.0f, dt * 10.0f);

    // --- Sway : l'arme traîne légèrement derrière la souris ---
    sway.x += (in.look.x * 0.18f - sway.x) * fminf(1.0f, dt * 8.0f);
    sway.y += (-in.look.y * 0.18f - sway.y) * fminf(1.0f, dt * 8.0f);
    sway.x = Clamp(sway.x, -0.03f, 0.03f);
    sway.y = Clamp(sway.y, -0.03f, 0.03f);
}

// Retour au repos des effets : le recul et le bloom redescendent tout
// seuls. "x -= x * min(1, dt*6)" = décroissance exponentielle (~6x par s).
static void UpdateViewEffects(float dt)
{
    kick = fmaxf(0, kick - dt * 12.0f);
    recoil_pitch -= recoil_pitch * fminf(1.0f, dt * 6.0f);
    recoil_yaw -= recoil_yaw * fminf(1.0f, dt * 6.0f);
    bloom = fmaxf(0, bloom - dt * 0.08f);
    muzzle_t -= dt;
    if (hit_marker_timer > 0) hit_marker_timer -= dt;
    if (damage_flash_timer > 0) damage_flash_timer -= dt;
    for (int i = 0; i < KILLFEED_MAX; i++)
        if (killfeed[i].ttl > 0) killfeed[i].ttl -= dt;
    for (int i = 0; i < TRACER_MAX; i++) {
        if (tracers[i].active) {
            tracers[i].ttl -= dt;
            if (tracers[i].ttl <= 0) tracers[i].active = false;
        }
    }
}

// Chaque personnage tourne la tête vers le joueur ENNEMI vivant le plus
// proche, nous compris (c'est ce qui rend un poussin zombie inquiétant
// quand il passe devant vous). Au-delà de 20 m il ne fixe plus personne et
// la tête revient doucement dans l'axe.
static void UpdateLookTargets(void)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        SnapshotPlayer *sp = &snap.players[i];
        if (!sp->active || i == my_id) continue;

        Vector3 self = display_pos[i];
        if (!sp->alive) {   // mort : plus de regard
            CharAnimLookAt(&anim[i], self, sp->yaw, self, false);
            continue;
        }

        Vector3 best = {0};
        float best_d2 = 400.0f;   // 20 m au carré
        bool  found = false;

        // Nous d'abord : notre position n'est pas dans display_pos
        if (my_alive && my_id >= 0 && snap.players[my_id].team != sp->team) {
            float dx = my_pos.x - self.x, dz = my_pos.z - self.z;
            float d2 = dx * dx + dz * dz;
            if (d2 < best_d2) {
                best_d2 = d2;
                best = (Vector3){my_pos.x, my_pos.y, my_pos.z};
                found = true;
            }
        }
        for (int j = 0; j < MAX_PLAYERS; j++) {
            SnapshotPlayer *o = &snap.players[j];
            if (j == i || j == my_id || !o->active || !o->alive) continue;
            if (o->team == sp->team) continue;
            float dx = display_pos[j].x - self.x, dz = display_pos[j].z - self.z;
            float d2 = dx * dx + dz * dz;
            if (d2 < best_d2) { best_d2 = d2; best = display_pos[j]; found = true; }
        }
        CharAnimLookAt(&anim[i], self, sp->yaw, best, found);
    }
}

// Les autres joueurs glissent vers leur dernière position reçue : les
// snapshots n'arrivent que 20 fois/s, sans lissage ils "téléporteraient".
static void InterpolateOthers(float dt)
{
    float k = 1.0f - expf(-INTERP_SPEED * dt);  // fraction du chemin restant
    for (int i = 0; i < MAX_PLAYERS; i++) {
        SnapshotPlayer *sp = &snap.players[i];
        if (!sp->active || i == my_id) { has_display[i] = false; continue; }
        Vector3 target = {sp->x, sp->y, sp->z};
        Vector3 before = display_pos[i];
        if (!has_display[i]) {
            display_pos[i] = target;   // première apparition : direct
            has_display[i] = true;
            before = target;
        } else if (Vector3Distance(display_pos[i], target) > 4.0f) {
            display_pos[i] = target;   // trop loin = respawn : téléportation
            before = target;
            CharAnimTeleport(&anim[i]);   // et son cadavre ne se relève pas
        } else {
            display_pos[i] = Vector3Lerp(display_pos[i], target, k);
        }

        // Toute l'animation se déduit du déplacement affiché cette image
        // (allure, foulée, saut, atterrissage) et de l'état vivant/mort :
        // le client n'a aucune donnée d'animation à recevoir du serveur.
        CharAnimUpdate(&anim[i], dt, before, display_pos[i], sp->alive != 0);
    }

    UpdateLookTargets();   // et chacun tourne la tête vers sa proie
}

// Caméra du joueur : aux yeux, avec head bob à la marche, recul, et FOV
// qui dépend de la visée/du sprint. À la mort, la caméra tombe au sol.
static void UpdatePlayerCamera(Camera3D *cam, float dt, float *fov_current)
{
    V3 eye = EyePos();

    if (!my_alive) {
        // Mort : la caméra descend à 35 cm du sol en une demi-seconde,
        // la tête penchée sur le côté.
        dead_t += dt;
        float k = fminf(dead_t * 2.0f, 1.0f);
        cam->position = (Vector3){eye.x, Lerp(eye.y, my_pos.y + 0.35f, k), eye.z};
        cam->target = (Vector3){eye.x + sinf(my_yaw), cam->position.y - 0.3f,
                                eye.z + cosf(my_yaw)};
        cam->up = Vector3Normalize((Vector3){cosf(my_yaw) * 0.6f, 1.0f, 0});
        cam->fovy = *fov_current;
        cam->projection = CAMERA_PERSPECTIVE;
        return;
    }

    // Head bob : rebond vertical proportionnel à la vitesse, presque
    // supprimé en visée pour garder la mire stable
    float eye_y = eye.y;
    if (on_ground) {
        float hspeed = sqrtf(my_vel.x * my_vel.x + my_vel.z * my_vel.z);
        eye_y += sinf(bob * 2.0f) * 0.03f
               * fminf(hspeed / PLAYER_SPEED, 1.4f) * (1.0f - ads * 0.8f);
    }

    V3 look = LookDir();   // recul compris : le tir fait vraiment monter la vue
    cam->position = (Vector3){eye.x, eye_y, eye.z};
    cam->target = (Vector3){eye.x + look.x, eye_y + look.y, eye.z + look.z};
    cam->up = (Vector3){0, 1, 0};
    cam->projection = CAMERA_PERSPECTIVE;

    // FOV cible selon la situation, rejoint en douceur
    float fov = Lerp(FOV_HIP, FOV_ADS, ads);
    fov = Lerp(fov, FOV_SPRINT, sprint_t);
    *fov_current += (fov - *fov_current) * fminf(1.0f, dt * 14.0f);
    cam->fovy = *fov_current;
}

//----------------------------------------------------------------------------------
// Rendu : map, joueurs, arme, HUD
//----------------------------------------------------------------------------------
static void DrawMap(void)
{
    // Sol de l'arène
    DrawPlane((Vector3){0, 0, 0},
              (Vector2){ARENA_HALF * 2, ARENA_HALF * 2},
              (Color){60, 65, 70, 255});
    // Quadrillage léger tous les 5 m (donne une échelle visuelle)
    for (int i = -(int)ARENA_HALF; i <= (int)ARENA_HALF; i += 5) {
        DrawLine3D((Vector3){(float)i, 0.01f, -ARENA_HALF},
                   (Vector3){(float)i, 0.01f, ARENA_HALF}, (Color){80, 85, 90, 255});
        DrawLine3D((Vector3){-ARENA_HALF, 0.01f, (float)i},
                   (Vector3){ARENA_HALF, 0.01f, (float)i}, (Color){80, 85, 90, 255});
    }
    // Les boîtes de la map (les mêmes AABB que celles des collisions :
    // ce qu'on voit est exactement ce qui bloque)
    for (int i = 0; i < MAP_BOX_COUNT; i++) {
        Box b = MAP_BOXES[i];
        Vector3 center = {(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2,
                          (b.min.z + b.max.z) / 2};
        Vector3 size = {b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z};
        DrawCubeV(center, size, (Color){110, 115, 125, 255});
        DrawCubeWiresV(center, size, (Color){40, 42, 48, 255});
    }
    // Enseigne reprise du premier jeu : un poussin géant tourne au bout d'un
    // mât planté sur le bloc central, repère visible de toute l'arène.
    // Matrice : échelle x6, rotation autour de Y, puis montée en haut du mât.
    // Hauteur du mât : le bloc culmine à 3 m et on peut monter dessus, donc
    // le poussin démarre à 5 m pour passer AU-DESSUS d'un joueur debout
    // (1,80 m) au lieu de lui remplir l'écran.
    float t = (float)GetTime();
    Seg(MatrixIdentity(), (Vector3){0, 3.0f, 0}, (Vector3){0, 5.0f, 0}, 0.07f,
        (Color){120, 120, 125, 255});
    DrawChick(MatrixMultiply(MatrixMultiply(MatrixScale(6, 6, 6), MatrixRotateY(t * 0.5f)),
                             MatrixTranslate(0, 5.0f, 0)), t, 0.2f);

    // Marquage au sol des zones de spawn de chaque équipe
    DrawCircle3D((Vector3){0, 0.02f, -26}, 4, (Vector3){1, 0, 0}, 90,
                 Fade(TeamColor(TEAM_RED), 0.4f));
    DrawCircle3D((Vector3){0, 0.02f, 26}, 4, (Vector3){1, 0, 0}, 90,
                 Fade(TeamColor(TEAM_BLUE), 0.4f));
}

// Les autres joueurs : un soldat-mercenaire pour les humains, un poussin
// zombie pour les bots (champ is_bot du snapshot). Les épaulettes/colliers
// prennent la couleur de l'équipe pour les reconnaître d'un coup d'œil.
static void DrawPlayers(void)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        SnapshotPlayer *sp = &snap.players[i];
        if (!sp->active || i == my_id) continue;
        // Un joueur mort reste dessiné le temps de s'effondrer PUIS de rester
        // au sol : son CharAnim garde death à 1 jusqu'au respawn. Les corps
        // ne disparaissent donc plus d'une image à l'autre.
        if (!sp->alive && anim[i].death <= 0.0f) continue;

        Vector3 p = display_pos[i];
        Color col = TeamColor(sp->team);

        if (sp->is_bot) DrawZombieChick(p, sp->yaw, &anim[i], col);
        else            DrawSoldier(p, sp->yaw, &anim[i], col);

        // Ombre "pastille" du premier jeu : elle se resserre quand il saute
        // et s'étale quand il est couché.
        DrawBlobShadow(p, 0.45f * (1.0f - anim[i].air * 0.3f) + anim[i].death * 0.18f);
    }

    for (int i = 0; i < TRACER_MAX; i++) {
        if (tracers[i].active)
            DrawLine3D(tracers[i].from, tracers[i].to, (Color){255, 230, 120, 255});
    }
}

// Modèle du fusil, décrit dans le repère de l'ARME : origine au boîtier,
// canon vers -Z, +Y vers le haut. Construit avec des formes simples.
// Dessiné via la pile de matrices rlgl : les DrawCube* sont transformés
// par la matrice courante (posée par DrawViewModel).
static void DrawRifleModel(void)
{
    const Color METAL = {38, 38, 36, 255};     // acier sombre
    const Color METAL2 = {55, 54, 50, 255};    // acier plus clair (contraste)
    const Color POLYMER = {30, 30, 30, 255};   // plastique (poignée, crosse)

    // Boîtier principal et partie basse
    DrawCube((Vector3){0, 0, 0}, 0.05f, 0.06f, 0.28f, METAL);
    DrawCube((Vector3){0, -0.048f, 0.02f}, 0.045f, 0.045f, 0.2f, METAL2);
    // Poignée pistolet (légèrement inclinée : approximée par une boîte)
    DrawCube((Vector3){0, -0.1f, 0.09f}, 0.036f, 0.1f, 0.05f, POLYMER);
    // Garde-main + rail supérieur
    DrawCube((Vector3){0, 0, -0.28f}, 0.055f, 0.055f, 0.3f, METAL2);
    DrawCube((Vector3){0, 0.034f, -0.12f}, 0.024f, 0.012f, 0.55f, METAL);
    // Canon et cache-flamme
    DrawCylinderEx((Vector3){0, 0, -0.43f}, (Vector3){0, 0, -0.58f},
                   0.012f, 0.012f, 8, METAL);
    DrawCylinderEx((Vector3){0, 0, -0.56f}, (Vector3){0, 0, -0.62f},
                   0.016f, 0.016f, 8, (Color){20, 20, 20, 255});
    // Crosse
    DrawCube((Vector3){0, -0.01f, 0.28f}, 0.045f, 0.08f, 0.16f, POLYMER);
    // Viseur : deux montants + point rouge au centre (aligné avec l'axe
    // de visée : en ADS il tombe exactement au centre de l'écran)
    DrawCube((Vector3){0, 0.065f, -0.04f}, 0.012f, 0.03f, 0.012f, POLYMER);
    DrawCube((Vector3){0, 0.065f, 0.02f}, 0.012f, 0.03f, 0.012f, POLYMER);
    DrawSphere((Vector3){0, 0.085f, -0.04f}, 0.004f, (Color){255, 40, 40, 255});

    // Chargeur courbé (deux boîtes)
    DrawCube((Vector3){0, -0.1f, -0.03f}, 0.03f, 0.1f, 0.06f, METAL2);
    DrawCube((Vector3){0, -0.16f, -0.045f}, 0.032f, 0.05f, 0.06f, METAL2);

    // Flash de bouche : boule + étoile de 4 branches, taille aléatoire à
    // chaque frame pour un effet vivant
    if (muzzle_t > 0) {
        float k = 0.8f + Randf(0, 0.6f);
        DrawSphere((Vector3){0, 0, -0.66f}, 0.03f * k, (Color){255, 240, 170, 255});
        DrawCube((Vector3){0, 0, -0.66f}, 0.14f * k, 0.012f, 0.012f,
                 (Color){255, 180, 60, 255});
        DrawCube((Vector3){0, 0, -0.66f}, 0.012f, 0.14f * k, 0.012f,
                 (Color){255, 180, 60, 255});
    }

    // Mains gantées : une sur le garde-main, une sur la poignée
    const Color GLOVE = {40, 38, 34, 255};
    DrawSphere((Vector3){-0.005f, -0.045f, -0.3f}, 0.035f, GLOVE);
    DrawSphere((Vector3){0.0f, -0.1f, 0.1f}, 0.035f, GLOVE);
}

// Passe de rendu du "view model" : l'arme est dessinée APRÈS la scène,
// avec une caméra fixe à l'origine et SANS test de profondeur : elle
// passe toujours devant le décor et ne rentre jamais dans les murs.
static void DrawViewModel(void)
{
    if (!my_alive) return;   // pas d'arme à l'écran quand on est mort

    Camera3D vc = {0};
    vc.position = (Vector3){0, 0, 0};
    vc.target = (Vector3){0, 0, -1};   // regarde vers -Z (convention espace vue)
    vc.up = (Vector3){0, 1, 0};
    // FOV propre à l'arme, indépendant de celui du monde : l'arme garde
    // une taille agréable et grossit légèrement en visant
    vc.fovy = Lerp(55.0f, 45.0f, ads);
    vc.projection = CAMERA_PERSPECTIVE;

    BeginMode3D(vc);
        rlDisableDepthTest();          // l'arme s'affiche par-dessus tout
        rlPushMatrix();
        // On pose le repère de l'arme (rotation + position en espace vue) :
        // tous les DrawCube de DrawRifleModel se font dedans
        Matrix m = WeaponViewMatrix();
        rlMultMatrixf(MatrixToFloat(m));
        DrawRifleModel();
        rlPopMatrix();
        rlEnableDepthTest();
    EndMode3D();
}

// Pseudos au-dessus des têtes (projection 3D -> écran)
static void DrawNameTags(Camera3D cam)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        SnapshotPlayer *sp = &snap.players[i];
        if (!sp->active || !sp->alive || i == my_id) continue;
        Vector3 head = {display_pos[i].x, display_pos[i].y + PLAYER_HEIGHT + 0.3f,
                        display_pos[i].z};
        Vector2 sc = GetWorldToScreen(head, cam);
        // GetWorldToScreen rend des pixels RÉELS : on repasse en unités
        // d'interface, puisque le pseudo est dessiné dans la caméra 2D.
        sc.x /= ui_scale;
        sc.y /= ui_scale;
        if (sc.x < 0 || sc.x > ui_w || sc.y < 0 || sc.y > ui_h) continue;
        // GetWorldToScreen projette aussi ce qui est DERRIÈRE la caméra :
        // on vérifie que le joueur est bien devant nous
        V3 look = LookDir();
        float dx = head.x - cam.position.x, dz = head.z - cam.position.z;
        if (dx * look.x + dz * look.z < 0) continue;
        int w = MeasureText(sp->name, 14);
        DrawText(sp->name, (int)sc.x - w / 2, (int)sc.y, 14,
                 Fade(TeamColor(sp->team), 0.9f));
    }
}

// Réticule dynamique : 4 traits qui s'écartent avec la dispersion réelle
// (WeaponSpread). Rouge pendant un hit. Quasi invisible en visée (c'est
// le point rouge du viseur qui sert de mire).
static void DrawCrosshair(void)
{
    if (ads > 0.7f) return;        // en visée : le viseur de l'arme suffit
    Color c = (hit_marker_timer > 0) ? RED : RAYWHITE;
    c = Fade(c, 1.0f - ads);       // s'efface pendant la transition de visée
    int cx = UIW / 2, cy = UIH / 2;
    // Écart en pixels proportionnel à la dispersion (~800 px par radian)
    int gap = 4 + (int)(WeaponSpread() * 800.0f);
    DrawLine(cx - gap - 6, cy, cx - gap, cy, c);
    DrawLine(cx + gap, cy, cx + gap + 6, cy, c);
    DrawLine(cx, cy - gap - 6, cx, cy - gap, c);
    DrawLine(cx, cy + gap, cx, cy + gap + 6, c);
}

static void DrawHUD(void)
{
    SnapshotPlayer *me = (my_id >= 0) ? &snap.players[my_id] : NULL;
    char buf[64];

    // --- Scores d'équipe en haut au centre ---
    snprintf(buf, sizeof(buf), "%d", snap.score[TEAM_RED]);
    DrawRectangle(UIW / 2 - 90, 12, 70, 34, Fade(TeamColor(TEAM_RED), 0.8f));
    DrawText(buf, UIW / 2 - 90 + 35 - MeasureText(buf, 24) / 2, 18, 24, WHITE);
    snprintf(buf, sizeof(buf), "%d", snap.score[TEAM_BLUE]);
    DrawRectangle(UIW / 2 + 20, 12, 70, 34, Fade(TeamColor(TEAM_BLUE), 0.8f));
    DrawText(buf, UIW / 2 + 20 + 35 - MeasureText(buf, 24) / 2, 18, 24, WHITE);

    // --- Vie et stats perso en bas à gauche ---
    if (me && me->active) {
        int hp = me->health < 0 ? 0 : me->health;
        DrawRectangle(28, UIH - 56, 204, 26, Fade(BLACK, 0.5f));
        Color hc = hp > 50 ? GREEN : (hp > 25 ? ORANGE : RED);
        DrawRectangle(30, UIH - 54, 2 * hp, 22, hc);   // 2 px par PV
        snprintf(buf, sizeof(buf), "%d PV", hp);
        DrawText(buf, 36, UIH - 52, 18, WHITE);
        snprintf(buf, sizeof(buf), "K %d / D %d", me->kills, me->deaths);
        DrawText(buf, 30, UIH - 82, 18, RAYWHITE);
    }

    DrawCrosshair();

    // --- Voile rouge quand on vient d'être touché ---
    if (damage_flash_timer > 0)
        DrawRectangle(0, 0, UIW, UIH,
                      Fade(RED, 0.25f * damage_flash_timer / 0.25f));

    // --- Killfeed en haut à droite ---
    int y = 60;
    for (int i = 0; i < KILLFEED_MAX; i++) {
        if (killfeed[i].ttl <= 0) continue;
        float a = killfeed[i].ttl > 1.0f ? 1.0f : killfeed[i].ttl;  // fondu en fin de vie
        int w = MeasureText(killfeed[i].text, 16);
        DrawText(killfeed[i].text, UIW - w - 20, y, 16, Fade(RAYWHITE, a));
        y += 22;
    }

    // --- Écran de mort ---
    if (me && me->active && !me->alive && !snap.match_over) {
        DrawRectangle(0, 0, UIW, UIH, Fade(BLACK, 0.5f));
        const char *msg = "Vous êtes mort — respawn imminent...";
        DrawText(msg, UIW / 2 - MeasureText(msg, 30) / 2,
                 UIH / 2 - 15, 30, RED);
    }

    // --- Fin de match ---
    if (snap.match_over) {
        DrawRectangle(0, 0, UIW, UIH, Fade(BLACK, 0.6f));
        const char *msg = (snap.winner == TEAM_RED)
            ? "VICTOIRE DE L'ÉQUIPE ROUGE" : "VICTOIRE DE L'ÉQUIPE BLEUE";
        DrawText(msg, UIW / 2 - MeasureText(msg, 40) / 2,
                 UIH / 2 - 20, 40, TeamColor(snap.winner));
    }

    // --- Tableau des scores (maintenu avec Tab) ---
    if (IsKeyDown(KEY_TAB)) {
        int px = UIW / 2 - 250, py = 100;
        DrawRectangle(px, py, 500, 400, Fade(BLACK, 0.75f));
        DrawText("ROUGE", px + 20, py + 15, 22, TeamColor(TEAM_RED));
        DrawText("BLEU", px + 270, py + 15, 22, TeamColor(TEAM_BLUE));
        int yr = py + 50, yb = py + 50;   // une colonne par équipe
        for (int i = 0; i < MAX_PLAYERS; i++) {
            SnapshotPlayer *sp = &snap.players[i];
            if (!sp->active) continue;
            snprintf(buf, sizeof(buf), "%-12.12s %3d / %-3d",
                     sp->name, sp->kills, sp->deaths);
            if (sp->team == TEAM_RED) {
                DrawText(buf, px + 20, yr, 18, RAYWHITE); yr += 24;
            } else {
                DrawText(buf, px + 270, yb, 18, RAYWHITE); yb += 24;
            }
        }
    }

    // --- Alerte si le serveur ne répond plus depuis 2 s ---
    if (GetTime() - last_server_msg > 2.0) {
        const char *msg = "Connexion au serveur perdue...";
        DrawText(msg, UIW / 2 - MeasureText(msg, 20) / 2, 60, 20, ORANGE);
    }
}

//----------------------------------------------------------------------------------
// Boucle principale
//----------------------------------------------------------------------------------
#if MOBILE_UI
//----------------------------------------------------------------------------------
// Écran de connexion du mobile
//----------------------------------------------------------------------------------
// Un téléphone n'a pas de ligne de commande : impossible de passer l'adresse
// du serveur en argument. On ouvre donc la fenêtre d'abord et on demande
// l'adresse au doigt, sur un pavé numérique (chiffres, point, effacement).
//
// raylib fait passer le tactile pour la souris sur Android : le même code
// marche sur PC, ce qui permet de régler cet écran sans téléphone.
//
// Remplit ip (cap octets) et renvoie true quand la connexion est établie,
// false si le joueur quitte l'application.
static bool MobileConnectScreen(char *ip, int cap, const char *name)
{
    // Pavé 3 x 4. Le « : » sert à viser un port autre que celui par défaut
    // (« 192.168.1.20:27015 ») ; l'effacement a son propre bouton, à droite
    // du champ de saisie.
    static const char *keys[12] = { "1","2","3","4","5","6","7","8","9",".","0",":" };
    const Color BG = {18, 20, 26, 255};
    char msg[80] = "";

    for (;;) {
        if (WindowShouldClose()) return false;
        UiUpdate();

        // Disposition, en unités d'interface : pavé 3 x 4 centré
        float bw = 112, bh = 78, gap = 12;
        float gx = ui_w / 2 - (3 * bw + 2 * gap) / 2, gy = 236;
        Rectangle connect = { ui_w / 2 - 140, gy + 4 * (bh + gap) + 14, 280, 78 };
        Rectangle field = { ui_w / 2 - 200, 150, 400, 62 };
        Rectangle erase = { field.x + field.width + 12, field.y, 76, field.height };

        // --- Un appui : on teste les boutons sous le doigt ---
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            Vector2 m = GetMousePosition();
            m.x /= ui_scale;      // les clics arrivent en pixels réels
            m.y /= ui_scale;
            for (int i = 0; i < 12; i++) {
                Rectangle r = { gx + (i % 3) * (bw + gap), gy + (i / 3) * (bh + gap), bw, bh };
                if (!CheckCollisionPointRec(m, r)) continue;
                size_t len = strlen(ip);
                if (len + 1 < (size_t)cap) { ip[len] = keys[i][0]; ip[len + 1] = '\0'; }
            }
            if (CheckCollisionPointRec(m, erase)) {
                size_t len = strlen(ip);
                if (len > 0) ip[len - 1] = '\0';
            }
            if (CheckCollisionPointRec(m, connect) && ip[0] != '\0') {
                // NetConnect bloque jusqu'à 5 s (10 tentatives) : on affiche
                // d'abord une image, sinon l'écran semble figé.
                BeginDrawing();
                ClearBackground(BG);
                BeginMode2D(UiCamera());
                const char *t = "Connexion...";
                DrawText(t, (int)(ui_w / 2) - MeasureText(t, 36) / 2, 320, 36, RAYWHITE);
                EndMode2D();
                EndDrawing();

                if (NetConnect(ip, name)) return true;
                snprintf(msg, sizeof(msg), "Serveur injoignable : %s", ip);
            }
        }

        // --- Dessin ---
        BeginDrawing();
        ClearBackground(BG);
        BeginMode2D(UiCamera());

        const char *title = "EMA TOURNAMENT";
        DrawText(title, (int)(ui_w / 2) - MeasureText(title, 44) / 2, 52, 44, RAYWHITE);
        const char *sub2 = "Adresse du serveur (port 7777 par defaut)";
        DrawText(sub2, (int)(ui_w / 2) - MeasureText(sub2, 20) / 2, 112, 20, GRAY);

        // Champ de saisie, et son bouton d'effacement
        DrawRectangleRec(field, Fade(BLACK, 0.5f));
        DrawRectangleLinesEx(field, 2, Fade(RAYWHITE, 0.4f));
        const char *shown = (ip[0] != '\0') ? ip : "___.___.___.___";
        DrawText(shown, (int)(field.x + field.width / 2) - MeasureText(shown, 34) / 2,
                 (int)field.y + 14, 34, ip[0] ? RAYWHITE : DARKGRAY);
        DrawRectangleRec(erase, Fade(RAYWHITE, 0.14f));
        DrawRectangleLinesEx(erase, 1, Fade(RAYWHITE, 0.3f));
        DrawText("<", (int)(erase.x + erase.width / 2) - MeasureText("<", 32) / 2,
                 (int)(erase.y + erase.height / 2) - 16, 32, RAYWHITE);

        // Pavé
        for (int i = 0; i < 12; i++) {
            Rectangle r = { gx + (i % 3) * (bw + gap), gy + (i / 3) * (bh + gap), bw, bh };
            DrawRectangleRec(r, Fade(RAYWHITE, 0.12f));
            DrawRectangleLinesEx(r, 1, Fade(RAYWHITE, 0.3f));
            DrawText(keys[i], (int)(r.x + r.width / 2) - MeasureText(keys[i], 32) / 2,
                     (int)(r.y + r.height / 2) - 16, 32, RAYWHITE);
        }

        // Bouton CONNECTER
        bool ready = (ip[0] != '\0');
        DrawRectangleRec(connect, Fade(ready ? (Color){70, 160, 90, 255} : GRAY, 0.8f));
        DrawRectangleLinesEx(connect, 2, Fade(RAYWHITE, 0.5f));
        const char *cl = "CONNECTER";
        DrawText(cl, (int)(connect.x + connect.width / 2) - MeasureText(cl, 30) / 2,
                 (int)(connect.y + 24), 30, RAYWHITE);

        if (msg[0] != '\0')
            DrawText(msg, (int)(ui_w / 2) - MeasureText(msg, 20) / 2,
                     (int)(connect.y + connect.height + 16), 20, (Color){235, 90, 70, 255});

        EndMode2D();
        EndDrawing();
    }
}
#endif  // MOBILE_UI

int main(int argc, char **argv)
{
#if !MOBILE_UI
    const char *ip = (argc > 1) ? argv[1] : "127.0.0.1";   // serveur à joindre
#endif
    const char *name = (argc > 2) ? argv[2] : "Joueur";

#if !MOBILE_UI
    // raylib moins bavard dans la console. PAS sur mobile : logcat y est le
    // seul canal de diagnostic, et c'est sur ses messages INFO (« Program
    // shader loaded ») que le test de fumee en CI verifie que l'affichage
    // s'est initialise.
    SetTraceLogLevel(LOG_WARNING);
#endif

#if MOBILE_UI
    // Mobile : la fenêtre d'abord (il faut un écran pour demander l'adresse
    // du serveur), et pas de capture du curseur — on a besoin du doigt.
  #if defined(PLATFORM_ANDROID)
    // 0 x 0 : raylib prend la taille NATIVE de l'écran. Avec 1280 x 720 il
    // rendrait en 16:9 et agrandirait l'image avec des bandes noires sur les
    // côtés (vu dans l'émulateur : « Upscaling required »). En natif, c'est
    // UiUpdate qui adapte le HUD au ratio réel de l'écran.
    InitWindow(0, 0, "EMA Tournament");
  #else
    InitWindow(SCREEN_W, SCREEN_H, "EMA Tournament");   // tactile simulé sur PC
  #endif
    SetTargetFPS(60);                // 60 Hz : suffisant, et ménage la batterie
    UiUpdate();
    char ip_typed[64] = "";
    if (!MobileConnectScreen(ip_typed, (int)sizeof(ip_typed), name)) {
        CloseWindow();
        return 0;
    }
#else
    printf("FPS TDM — connexion à %s en tant que \"%s\"\n", ip, name);
    // PC : on se connecte AVANT d'ouvrir la fenêtre, inutile de l'ouvrir si
    // le serveur est injoignable
    if (!NetConnect(ip, name)) return 1;

    InitWindow(SCREEN_W, SCREEN_H, "EMA Tournament — TDM");
    SetTargetFPS(144);
    DisableCursor();   // capture la souris (visée FPS)
#endif

    ModelsInit();   // crée les maillages des personnages (après InitWindow)
    // Un état d'animation par joueur, décalé par son numéro de slot : deux
    // poussins ne clignent pas des yeux au même instant et ne s'ébrouent
    // pas ensemble.
    for (int i = 0; i < MAX_PLAYERS; i++) CharAnimInit(&anim[i], i + 1);
    UiUpdate();
    TouchReset();

    // Position de départ : premier spawn de notre équipe. Le premier
    // snapshot du serveur corrigera si besoin.
    my_pos = TEAM_SPAWNS[my_team][0];
    my_yaw = (my_team == TEAM_RED) ? 0.0f : (float)M_PI;   // face au centre
    memset(&snap, 0, sizeof(snap));

    double last_state_send = 0.0;
    float fov_current = FOV_HIP;
    Camera3D cam = {0};

    while (!WindowShouldClose()) {
        // dt plafonné : après un gel (fenêtre déplacée...), pas de pas de
        // physique géant qui ferait traverser les murs
        float dt = GetFrameTime();
        if (dt > 0.05f) dt = 0.05f;

        UiUpdate();                         // l'écran a pu tourner (mobile)
        TouchUpdate(ui_scale, ui_w, ui_h);  // doigts posés cette image
        NetReceive();                       // snapshots + événements

        if (my_alive && !snap.match_over) UpdateLocalPlayer(dt);
        UpdateViewEffects(dt);              // recul, bloom, minuteurs...
        InterpolateOthers(dt);              // lissage des autres joueurs
        UpdatePlayerCamera(&cam, dt, &fov_current);

        // Envoi périodique de notre position au serveur
        if (GetTime() - last_state_send > 1.0 / CLIENT_SEND_HZ) {
            last_state_send = GetTime();
            NetSendState();
        }

        BeginDrawing();
        ClearBackground((Color){135, 170, 200, 255});   // ciel

        BeginMode3D(cam);       // 1) la scène : map + joueurs + traçantes
        DrawMap();
        DrawPlayers();
        EndMode3D();

        DrawViewModel();        // 2) l'arme, par-dessus la scène

        // 3) l'interface, en unités d'interface : la caméra 2D la met à
        // l'échelle de l'écran réel (indispensable sur mobile).
        BeginMode2D(UiCamera());
        DrawNameTags(cam);
        DrawHUD();
        TouchDraw();            // joystick et boutons (mobile seulement)
        DrawFPS(UIW - 90, UIH - 30);
        EndMode2D();
        EndDrawing();
    }

    NetSendDisconnect();
    ModelsClose();     // libère les maillages (avant CloseWindow)
    CloseWindow();
    SocketClose(sock_fd);
    PlatformNetShutdown();
    return 0;
}
