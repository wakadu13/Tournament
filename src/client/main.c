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
#include "weapons.h"  // modèles 3D des armes
#include "touch.h"    // commandes tactiles (mobile) et structure PlayerInput
#include "options.h"  // réglages persistants (options.cfg)
#include "ui.h"       // widgets des menus
#include "menu.h"     // accueil, pause, équipement, options

//----------------------------------------------------------------------------------
// Réglages de la vue FPS
//----------------------------------------------------------------------------------
#define SCREEN_W 1280
#define SCREEN_H 720

#define MOUSE_SENSITIVITY 0.0025f  // radians par pixel de souris (x options.sensitivity)

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
// la cible en douceur à chaque frame (pas de saut brutal). La base est
// réglable (options.fov) ; la visée zoome de 25°, la lunette bien plus.
#define FOV_HIP    (options.fov)             // normal, arme à la hanche
#define FOV_ADS    (options.fov - 25.0f)     // en visant : effet de zoom
#define FOV_SCOPE  12.0f                     // dans la lunette du FR-F2
#define FOV_SPRINT (options.fov + 9.0f)      // en sprint : impression de vitesse


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
// Notre simulation (position des pieds, vitesses, au sol) : le MÊME code
// que le serveur (PlayerStep, game.c). On la fait tourner en avance sur lui
// (prédiction) et on se recale sur ses snapshots (réconciliation).
static PlayerSim my_sim;
static float my_yaw = 0.0f;        // regard gauche/droite (radians)
static float my_pitch = 0.0f;      // regard haut/bas (radians)
static bool  my_alive = true;      // décidé par le serveur
static uint8_t my_spawn_count = 0; // compteur de respawn (voir protocol.h)
static bool  sprinting = false;
static float dead_t = 0.0f;        // temps écoulé depuis notre mort (anim caméra)
static int   last_health = PLAYER_MAX_HEALTH;

static void SnapPush(const PacketSnapshot *sp);
static void Reconcile(const SnapshotPlayer *me);

//----------------------------------------------------------------------------------
// Prédiction et réconciliation
//----------------------------------------------------------------------------------
// Chaque image produit UNE entrée (déplacement, vue, saut...), simulée
// tout de suite et gardée ici jusqu'à ce que le serveur l'acquitte
// (champ last_seq du snapshot). À chaque snapshot on repart de l'état que
// le serveur nous donne et on rejoue les entrées qu'il n'a pas encore vues.
#define PENDING_MAX 256                    // ~1,8 s à 144 images/s : largement plus qu'un ping
static InputEntry pending[PENDING_MAX];    // anneau : head = plus ancienne
static int        pending_head = 0, pending_count = 0;
static uint16_t   my_seq = 0;              // numéro de la dernière entrée produite

static InputEntry PendingAt(int i) { return pending[(pending_head + i) % PENDING_MAX]; }
static void PendingPush(const InputEntry *e)
{
    if (pending_count == PENDING_MAX) {    // plein : on oublie la plus ancienne
        pending_head = (pending_head + 1) % PENDING_MAX;
        pending_count--;
    }
    pending[(pending_head + pending_count) % PENDING_MAX] = *e;
    pending_count++;
}
// Retire toutes les entrées jusqu'à seq compris (différence signée sur
// 16 bits : gère le bouclage du compteur)
static void PendingAck(uint16_t seq)
{
    while (pending_count > 0 && (int16_t)(PendingAt(0).seq - seq) <= 0) {
        pending_head = (pending_head + 1) % PENDING_MAX;
        pending_count--;
    }
}

//----------------------------------------------------------------------------------
// Interpolation des autres joueurs
//----------------------------------------------------------------------------------
// Les snapshots n'arrivent que 20 fois/s : on affiche les autres joueurs
// INTERP_DELAY dans le passé, entre les deux snapshots qui encadrent cet
// instant. Le mouvement est fluide et, surtout, ce qu'on voit correspond à
// un instant serveur PRÉCIS : c'est lui qu'on envoie avec nos tirs, et le
// serveur rembobine les cibles à cet instant (compensation de lag).
#define SNAP_BUF 32
typedef struct { PacketSnapshot snap; bool valid; } SnapEntry;
static SnapEntry snapbuf[SNAP_BUF];        // anneau indexé par le tick
static double server_offset = 0.0;         // temps serveur ~= GetTime() + server_offset
static bool   have_offset = false;
static float  view_time = 0.0f;            // instant serveur affiché cette image
static float  display_yaw[MAX_PLAYERS];    // orientation interpolée

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
// Armes (côté client)
//----------------------------------------------------------------------------------
// Le serveur tient les chargeurs (voir server/main.c). Ici on garde ce
// qu'il nous a dit au dernier snapshot, moins nos tirs encore en route,
// et des minuteurs LOCAUX de rechargement / changement d'arme pour
// l'animation et pour ne pas tirer dans le vide (le serveur les refuserait).
static int      my_weapon[WEAPON_SLOTS] = { WPN_FAMAS, WPN_GLOCK };  // ce qu'on tient
static int      my_slot = SLOT_PRIMARY;      // emplacement actif
static int      my_mag[WEAPON_SLOTS];        // cartouches affichées (snapshot - tirs en route)
static int      my_reserve[WEAPON_SLOTS];
static uint16_t my_shot_seq = 0;             // numéro de notre dernier tir
static float    reload_t = 0.0f;             // rechargement local (s restantes)
static float    reload_total = 1.0f;         // sa durée, pour l'animation
static float    switch_t = 0.0f;             // changement d'arme local (s restantes)
static float    bolt_t = 0.0f;               // réarmement (verrou, pompe, glissière) : 1 -> 0
static bool     fire_was_down = false;       // front du bouton de tir (coup par coup)
static char     weapon_msg[96] = "";         // « équipement appliqué » ...
static float    weapon_msg_t = 0.0f;
static bool     scoped = false;              // dans la lunette (FR-F2 en visée)

// Menu ouvert en jeu (pause, armes, options) ; MENU_NONE = on joue
static MenuScreen menu = MENU_NONE;
static bool       quit_requested = false;

static const WeaponStats *CurWeapon(void) { return &WEAPONS[my_weapon[my_slot]]; }

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

        // On écoute la réponse pendant une demi-seconde. PlatformNow et
        // pas GetTime : sur PC on peut se connecter AVANT d'ouvrir la
        // fenêtre, et l'horloge de raylib ne démarre qu'avec elle.
        double deadline = PlatformNow() + 0.5;
        while (PlatformNow() < deadline) {
            uint8_t buf[2048];
            sock_ssize_t n = recvfrom(sock_fd, buf, sizeof(buf), 0, NULL, NULL);
            if (n >= (sock_ssize_t)sizeof(PacketAccept) && buf[0] == PKT_SV_ACCEPT) {
                PacketAccept *acc = (PacketAccept *)buf;
                my_id = acc->player_id;       // le serveur nous donne notre slot
                my_team = (Team)acc->team;    // et notre équipe (auto-équilibrage)
                last_server_msg = -1.0;       // posé à la première image de jeu
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
// Envoie les INPUT_BATCH dernières entrées (ou moins). Les mêmes repartent
// dans plusieurs paquets de suite : si l'un se perd, le suivant les porte.
// Sans entrée en attente (mort, fin de match), le paquet part vide : il
// sert alors de battement de cœur contre le timeout du serveur.
static void NetSendInputs(void)
{
    PacketClientInput pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PKT_CL_INPUT;
    pkt.player_id = (uint8_t)my_id;
    pkt.spawn_count = my_spawn_count;
    int n = (pending_count < INPUT_BATCH) ? pending_count : INPUT_BATCH;
    for (int i = 0; i < n; i++) pkt.entries[i] = PendingAt(pending_count - n + i);
    pkt.count = (uint8_t)n;
    sendto(sock_fd, &pkt, sizeof(pkt), 0,
           (struct sockaddr *)&server_addr, sizeof(server_addr));
}

// Envoie un tir : le rayon (origine + direction) part de nos yeux.
// C'est le SERVEUR qui décide si quelqu'un est touché, pas nous.
static void NetSendShoot(V3 origin, V3 dir)
{
    PacketShoot pkt = {
        PKT_CL_SHOOT, (uint8_t)my_id,
        (uint8_t)my_slot, 0, ++my_shot_seq,   // l'arme avec laquelle on tire, et le numéro du tir
        origin.x, origin.y, origin.z,
        dir.x, dir.y, dir.z,
        view_time,                 // l'instant qu'on voyait : le serveur y rembobine les cibles
    };
    sendto(sock_fd, &pkt, sizeof(pkt), 0,
           (struct sockaddr *)&server_addr, sizeof(server_addr));
}

// Équipement, changement d'arme, rechargement : le serveur fait autorité
// (voir HandleWeapon côté serveur), on anime en attendant sa réponse.
static void NetSendWeapon(int action, int a, int b)
{
    PacketWeapon pkt = { PKT_CL_WEAPON, (uint8_t)my_id, (uint8_t)action, (uint8_t)a, (uint8_t)b };
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
    case EV_KILL: {
        int w = ev->value & 0xFF;
        bool head = (ev->value & 0x100) != 0;
        snprintf(line, sizeof(line), "%s a éliminé %s (%s%s)",
                 PlayerName(ev->a), PlayerName(ev->b),
                 (w >= 0 && w < WPN_COUNT) ? WEAPONS[w].name : "?", head ? ", tête" : "");
        KillfeedPush(line);
    }
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
    case EV_ROUND_START:
        snprintf(line, sizeof(line), "Manche %d", ev->a);
        KillfeedPush(line);
        break;
    case EV_ROUND_END:
        if (ev->a < TEAM_COUNT)
            snprintf(line, sizeof(line), "Manche %d : %s l'emporte", ev->b,
                     ev->a == TEAM_RED ? "ROUGE" : "BLEU");
        else
            snprintf(line, sizeof(line), "Manche %d : nulle", ev->b);
        KillfeedPush(line);
        break;
    case EV_PLAYER_JOIN:
        snprintf(line, sizeof(line), "%s a rejoint la partie", PlayerName(ev->a));
        KillfeedPush(line);
        break;
    case EV_PLAYER_LEAVE:
        KillfeedPush("un joueur est parti");
        break;
    case EV_LOADOUT:
        snprintf(weapon_msg, sizeof(weapon_msg), ev->value
                 ? "Équipement appliqué" : "Équipement prêt pour la prochaine manche");
        weapon_msg_t = 3.0f;
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
            const PacketSnapshot *in = (const PacketSnapshot *)buf;
            // Un snapshot plus vieux que le dernier reçu (UDP réordonné) va
            // dans le tampon d'interpolation mais ne devient pas « le dernier »
            if (have_offset && (int32_t)(in->tick - snap.tick) <= 0) {
                SnapPush(in);
                continue;
            }
            memcpy(&snap, buf, sizeof(snap));
            SnapPush(&snap);

            // Le serveur fait autorité sur notre vie. Quand son compteur
            // de respawn change (y compris au tout premier spawn), on
            // reprend la position qu'IL a choisie, et on écho le nouveau
            // compteur dans nos paquets d'entrées.
            SnapshotPlayer *me = &snap.players[my_id];
            if (me->active) {
                if (me->spawn_count != my_spawn_count) {   // (re)spawn
                    my_spawn_count = me->spawn_count;
                    memset(&my_sim, 0, sizeof(my_sim));
                    my_sim.pos = (V3){me->x, me->y, me->z};
                    my_sim.on_ground = me->on_ground != 0;
                    my_yaw = me->yaw;
                    my_pitch = me->pitch;
                    pending_head = 0;        // tout ce qui précède est périmé
                    pending_count = 0;
                    dead_t = 0.0f;
                    // Nouvel équipement en main, arme principale, rien en cours
                    my_slot = me->slot;
                    reload_t = switch_t = bolt_t = 0.0f;
                } else if (me->alive) {
                    Reconcile(me);
                }
                my_alive = me->alive != 0;
                // Armes : ce que le serveur nous donne, moins les tirs qu'il
                // n'a pas encore vus (différence signée : gère le bouclage)
                for (int k = 0; k < WEAPON_SLOTS; k++) {
                    my_weapon[k] = me->weapon[k];
                    my_mag[k] = me->mag[k];
                    my_reserve[k] = me->reserve[k];
                }
                int in_flight = (int16_t)(my_shot_seq - me->last_shot_seq);
                if (in_flight > 0 && WEAPONS[my_weapon[my_slot]].mag > 0) {
                    my_mag[my_slot] -= in_flight;
                    if (my_mag[my_slot] < 0) my_mag[my_slot] = 0;
                }
                // Le serveur a fini de recharger avant nous (ou ne l'a jamais
                // commencé) : on s'aligne
                if (me->reload_t <= 0.0f && reload_t > 0.0f && reload_t < 0.2f) reload_t = 0.0f;
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
    return (V3){my_sim.pos.x, my_sim.pos.y + PLAYER_EYE_HEIGHT, my_sim.pos.z};
}

// Dispersion des balles : rayon du cône de tir (~radians). Dépend de :
//   - la visée : 0.022 à la hanche -> 0.002 en visant,
//   - la vitesse de déplacement (courir rend imprécis),
//   - le saut (+0.04 en l'air),
//   - le bloom, qui grossit à chaque tir et redescend avec le temps.
// Le réticule du HUD s'écarte avec cette même valeur.
static float WeaponSpread(void)
{
    const WeaponStats *w = CurWeapon();
    float hspeed = sqrtf(my_sim.vel.x * my_sim.vel.x + my_sim.vel.z * my_sim.vel.z);
    return Lerp(w->spread_hip, w->spread_ads, ads)
         + hspeed * 0.003f
         + (my_sim.on_ground ? 0.0f : 0.04f)
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

// Bout du canon en espace vue (dépend du modèle de l'arme)
static Vector3 MuzzleView(void)
{
    return Vector3Transform(WeaponMuzzle(my_weapon[my_slot]), WeaponViewMatrix());
}

// Une traçante (purement visuelle) de notre canon jusqu'au premier
// obstacle dans la direction dir : mur, ou joueur affiché.
static void TracerFrom(V3 eye, Vector3 d)
{
    V3 dir = {d.x, d.y, d.z};
    float t = RaycastMap(eye, dir);
    if (t > CurWeapon()->range) t = CurWeapon()->range;
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

// Tire un coup avec l'arme en main : recul, flash, dispersion, envoi au
// serveur et traçante(s). Le serveur décide des dégâts.
static void Shoot(void)
{
    const WeaponStats *w = CurWeapon();
    shoot_cooldown = w->cooldown;
    muzzle_t = 0.05f;              // flash de bouche pendant 50 ms
    kick = (w->pellets > 1 || w->mode == FIRE_BOLT) ? 1.6f : 1.0f;   // recul visuel, plus fort sur les grosses armes
    // Le canon monte (moins en visant) et dévie un peu au hasard. Il
    // redescend tout seul (décroissance dans UpdateViewEffects).
    recoil_pitch += Randf(w->recoil * 0.8f, w->recoil * 1.2f) * (1.0f - ads * 0.35f);
    recoil_yaw += Randf(-w->recoil * 0.5f, w->recoil * 0.5f);
    bloom = fminf(bloom + w->recoil * 0.6f, 0.04f);   // rafale = moins précis (plafonné)
    if (w->mag > 0 && my_mag[my_slot] > 0) my_mag[my_slot]--;   // en attendant le snapshot
    if (w->mode == FIRE_BOLT || w->mode == FIRE_PUMP) bolt_t = 1.0f;   // réarmement animé
    else if (w->mode == FIRE_SEMI) bolt_t = 0.6f;                      // glissière du pistolet

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
    NetSendShoot(eye, (V3){d.x, d.y, d.z});

    // Traçantes : une par balle, ou une gerbe pour le fusil à pompe (le
    // serveur tire ses propres plombs : la gerbe affichée est indicative)
    if (w->pellets <= 1) {
        TracerFrom(eye, d);
    } else {
        for (int i = 0; i < w->pellets; i++) {
            float a = Randf(-0.035f, 0.035f), b = Randf(-0.035f, 0.035f);
            Vector3 pd = Vector3Normalize((Vector3){
                d.x + r.x * a + u.x * b, d.y + r.y * a + u.y * b, d.z + r.z * a + u.z * b});
            TracerFrom(eye, pd);
        }
    }
}

// Changement d'arme : local tout de suite (animation, arme affichée), et
// le serveur suit. Pendant WEAPON_SWITCH_TIME on ne peut pas tirer.
static void SwitchWeapon(int slot)
{
    if (slot == my_slot || slot < 0 || slot >= WEAPON_SLOTS || switch_t > 0.0f) return;
    my_slot = slot;
    switch_t = WEAPON_SWITCH_TIME + 0.05f;    // un peu plus que le serveur : il refuserait un tir trop tôt
    reload_t = 0.0f;
    bolt_t = 0.0f;
    NetSendWeapon(WA_SWITCH, slot, 0);
}

static void StartReload(void)
{
    const WeaponStats *w = CurWeapon();
    if (reload_t > 0.0f || switch_t > 0.0f || w->mag == 0) return;
    if (my_mag[my_slot] >= w->mag || my_reserve[my_slot] <= 0) return;
    reload_total = w->reload;
    reload_t = w->reload + 0.05f;
    NetSendWeapon(WA_RELOAD, 0, 0);
}

// Envoie l'équipement choisi dans les options ; le serveur dira s'il
// s'applique tout de suite ou à la prochaine manche (EV_LOADOUT).
static void SendLoadout(void)
{
    NetSendWeapon(WA_LOADOUT, options.primary, options.secondary);
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
    float sens = MOUSE_SENSITIVITY * options.sensitivity * (1.0f - ads * 0.35f) * (scoped ? 0.4f : 1.0f);
    Vector2 md = GetMouseDelta();
    in.look.x = -md.x * sens;
    in.look.y = -md.y * sens;
    in.fire = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    in.fire_pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    in.aim  = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
    in.reload = IsKeyPressed(KEY_R);
    // Changement d'arme : molette, ou 1 / 2 (positions des touches)
    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) in.swap = true;
    if (IsKeyPressed(KEY_ONE)) SwitchWeapon(SLOT_PRIMARY);
    if (IsKeyPressed(KEY_TWO)) SwitchWeapon(SLOT_SECONDARY);
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
    if (in.swap) SwitchWeapon(my_slot == SLOT_PRIMARY ? SLOT_SECONDARY : SLOT_PRIMARY);
    if (in.reload) StartReload();
    if (options.invert_y) in.look.y = -in.look.y;

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

    // --- Déplacement : UNE entrée pour cette image, simulée tout de suite
    // (prédiction) et gardée jusqu'à l'acquittement du serveur. Le
    // déplacement est quantifié sur un octet AVANT de simuler : le serveur
    // rejouera exactement la même valeur. ---
    InputEntry e;
    memset(&e, 0, sizeof(e));
    e.seq = ++my_seq;
    e.dt = (dt > INPUT_MAX_DT) ? INPUT_MAX_DT : dt;
    e.move_x = (int8_t)lroundf(move.x * 127.0f);
    e.move_y = (int8_t)lroundf(move.y * 127.0f);
    e.flags = (uint8_t)((in.jump ? INPUT_JUMP : 0)
                      | (sprinting ? INPUT_SPRINT : 0)
                      | ((aiming && !sprinting) ? INPUT_ADS : 0));
    e.yaw = my_yaw;
    e.pitch = my_pitch;
    PlayerCmd cmd = CmdFromEntry(&e);
    cmd.speed_mult = CurWeapon()->move_mult;   // comme le serveur : l'arme pèse
    PlayerStep(&my_sim, &cmd, e.dt);      // collisions et gravité : code partagé
    PendingPush(&e);

    // Phase du balancement de marche : avance avec la vitesse au sol
    float hspeed = sqrtf(my_sim.vel.x * my_sim.vel.x + my_sim.vel.z * my_sim.vel.z);
    if (my_sim.on_ground) bob += dt * hspeed * 1.6f;

    // --- Tir : selon le mode de l'arme (automatique tant que le bouton est
    // maintenu, ou un coup par pression), à sa cadence, jamais pendant un
    // sprint, un rechargement ou un changement d'arme, et chargeur non vide ---
    const WeaponStats *w = CurWeapon();
    if (shoot_cooldown > 0.0f) shoot_cooldown -= dt;
    bool trigger = (w->mode == FIRE_AUTO) ? firing : (firing && !fire_was_down) || in.fire_pressed;
    fire_was_down = firing;
    bool can_fire = !sprinting && shoot_cooldown <= 0.0f && reload_t <= 0.0f && switch_t <= 0.0f;
    if (trigger && can_fire) {
        if (w->mag == 0 || my_mag[my_slot] > 0) Shoot();
        else if (options.auto_reload) StartReload();
    }
    // Chargeur vide et plus personne n'appuie : on recharge aussi
    if (options.auto_reload && w->mag > 0 && my_mag[my_slot] == 0 && my_reserve[my_slot] > 0
        && reload_t <= 0.0f && switch_t <= 0.0f && shoot_cooldown <= 0.0f)
        StartReload();

    // --- Transitions douces de la visée et du sprint (0 -> 1) ---
    ads += ((aiming && !sprinting ? 1.0f : 0.0f) - ads) * fminf(1.0f, dt * 14.0f);
    scoped = WeaponHasScope(my_weapon[my_slot]) && ads > 0.9f;
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
    if (reload_t > 0.0f) reload_t -= dt;
    if (switch_t > 0.0f) switch_t -= dt;
    if (bolt_t > 0.0f) bolt_t = fmaxf(0, bolt_t - dt * 2.2f);
    if (weapon_msg_t > 0.0f) weapon_msg_t -= dt;
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
            CharAnimLookAt(&anim[i], self, display_yaw[i], self, false);
            continue;
        }

        Vector3 best = {0};
        float best_d2 = 400.0f;   // 20 m au carré
        bool  found = false;

        // Nous d'abord : notre position n'est pas dans display_pos
        if (my_alive && my_id >= 0 && snap.players[my_id].team != sp->team) {
            float dx = my_sim.pos.x - self.x, dz = my_sim.pos.z - self.z;
            float d2 = dx * dx + dz * dz;
            if (d2 < best_d2) {
                best_d2 = d2;
                best = (Vector3){my_sim.pos.x, my_sim.pos.y, my_sim.pos.z};
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
        CharAnimLookAt(&anim[i], self, display_yaw[i], best, found);
    }
}

// Range un snapshot dans l'anneau et affine l'horloge serveur estimée.
static void SnapPush(const PacketSnapshot *sp)
{
    SnapEntry *e = &snapbuf[sp->tick % SNAP_BUF];
    e->snap = *sp;
    e->valid = true;
    // temps serveur du snapshot - instant de réception : l'écart entre nos
    // deux horloges, lissé (il contient la gigue), repris d'un coup s'il
    // décroche (pause, reconnexion)
    double off = (double)sp->tick / SERVER_TICK_HZ - GetTime();
    if (!have_offset || fabs(off - server_offset) > 0.5) { server_offset = off; have_offset = true; }
    else server_offset += (off - server_offset) * 0.05;
}

// Réconciliation : le serveur nous dit où il nous a mis après notre entrée
// last_seq. On repart de là et on rejoue nos entrées plus récentes. Si nos
// prédictions étaient justes (presque toujours), on retombe au même
// endroit ; sinon la correction du serveur s'applique, et il a raison.
static void Reconcile(const SnapshotPlayer *me)
{
    PendingAck(me->last_seq);
    PlayerSim s;
    memset(&s, 0, sizeof(s));
    s.pos = (V3){me->x, me->y, me->z};
    s.vel = (V3){me->vx, 0, me->vz};
    s.vel_y = me->vy;
    s.on_ground = me->on_ground != 0;
    for (int i = 0; i < pending_count; i++) {
        InputEntry e = PendingAt(i);
        PlayerCmd c = CmdFromEntry(&e);
        PlayerStep(&s, &c, e.dt);
    }
    my_sim = s;
}

// Interpolation d'un angle par le chemin le plus court
static float LerpAngle(float a, float b, float t)
{
    float d = b - a;
    while (d >  PI) d -= 2.0f * PI;
    while (d < -PI) d += 2.0f * PI;
    return a + d * t;
}

// Les autres joueurs sont affichés INTERP_DELAY dans le passé, entre les
// deux snapshots qui encadrent cet instant (voir SnapEntry). S'il manque
// le snapshot suivant (à-coup réseau), on reste sur le dernier connu : on
// n'extrapole jamais.
static void InterpolateOthers(float dt)
{
    if (!have_offset) return;
    double render = GetTime() + server_offset - INTERP_DELAY;
    view_time = (float)render;

    const SnapEntry *a = NULL, *b = NULL;    // a : le plus récent <= render ; b : le plus ancien > render
    double ta = 0, tb = 0;
    for (int i = 0; i < SNAP_BUF; i++) {
        if (!snapbuf[i].valid) continue;
        double t = (double)snapbuf[i].snap.tick / SERVER_TICK_HZ;
        if (t <= render && (!a || t > ta)) { a = &snapbuf[i]; ta = t; }
        if (t >  render && (!b || t < tb)) { b = &snapbuf[i]; tb = t; }
    }
    if (!a && !b) return;
    if (!a) { a = b; ta = tb; }
    if (!b) { b = a; tb = ta; }
    float alpha = (tb > ta) ? (float)((render - ta) / (tb - ta)) : 0.0f;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        SnapshotPlayer *sp = &snap.players[i];          // drapeaux : le dernier snapshot
        if (!sp->active || i == my_id) { has_display[i] = false; continue; }
        const SnapshotPlayer *pa = &a->snap.players[i], *pb = &b->snap.players[i];
        if (!pa->active) pa = pb;
        if (!pb->active) pb = pa;

        Vector3 target;
        float yaw;
        if (!pa->active) { target = (Vector3){sp->x, sp->y, sp->z}; yaw = sp->yaw; }
        else if (pa->spawn_count != pb->spawn_count) {
            // Respawn entre les deux : pas d'interpolation à travers l'arène
            target = (Vector3){pb->x, pb->y, pb->z}; yaw = pb->yaw;
        } else {
            target = Vector3Lerp((Vector3){pa->x, pa->y, pa->z}, (Vector3){pb->x, pb->y, pb->z}, alpha);
            yaw = LerpAngle(pa->yaw, pb->yaw, alpha);
        }

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
            display_pos[i] = target;
        }
        display_yaw[i] = yaw;

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
        cam->position = (Vector3){eye.x, Lerp(eye.y, my_sim.pos.y + 0.35f, k), eye.z};
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
    if (my_sim.on_ground) {
        float hspeed = sqrtf(my_sim.vel.x * my_sim.vel.x + my_sim.vel.z * my_sim.vel.z);
        eye_y += sinf(bob * 2.0f) * 0.03f
               * fminf(hspeed / PLAYER_SPEED, 1.4f) * (1.0f - ads * 0.8f);
    }

    V3 look = LookDir();   // recul compris : le tir fait vraiment monter la vue
    cam->position = (Vector3){eye.x, eye_y, eye.z};
    cam->target = (Vector3){eye.x + look.x, eye_y + look.y, eye.z + look.z};
    cam->up = (Vector3){0, 1, 0};
    cam->projection = CAMERA_PERSPECTIVE;

    // FOV cible selon la situation, rejoint en douceur
    float fov = Lerp(FOV_HIP, WeaponHasScope(my_weapon[my_slot]) ? FOV_SCOPE : FOV_ADS, ads);
    fov = Lerp(fov, FOV_SPRINT, sprint_t);
    *fov_current += (fov - *fov_current) * fminf(1.0f, dt * 14.0f);
    cam->fovy = *fov_current;
}

//----------------------------------------------------------------------------------
// Rendu : map, joueurs, arme, HUD
//----------------------------------------------------------------------------------
static void DrawMap(void)
{
    // Sol de l'arène : sable (la carte est inspirée de Dust)
    DrawPlane((Vector3){0, 0, 0},
              (Vector2){ARENA_HALF * 2, ARENA_HALF * 2},
              (Color){186, 160, 112, 255});
    // Quadrillage léger tous les 10 m (donne une échelle visuelle)
    for (int i = -(int)ARENA_HALF; i <= (int)ARENA_HALF; i += 10) {
        DrawLine3D((Vector3){(float)i, 0.01f, -ARENA_HALF},
                   (Vector3){(float)i, 0.01f, ARENA_HALF}, (Color){170, 146, 100, 255});
        DrawLine3D((Vector3){-ARENA_HALF, 0.01f, (float)i},
                   (Vector3){ARENA_HALF, 0.01f, (float)i}, (Color){170, 146, 100, 255});
    }
    // Les boîtes de la map (les mêmes AABB que celles des collisions : ce
    // qu'on voit est exactement ce qui bloque). Elles passent par le lot
    // (Box3) : un seul appel de dessin avec les personnages, et l'ombrage
    // cuit par face donne du relief aux murs. Couleur selon le rôle :
    // pisé pour les murs, bois pour les caisses, pierre pour les toits.
    for (int i = 0; i < MAP_BOX_COUNT; i++) {
        Box b = MAP_BOXES[i];
        Vector3 center = {(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2,
                          (b.min.z + b.max.z) / 2};
        Vector3 size = {b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z};
        Color c = (Color){212, 190, 142, 255};                     // pisé
        if (b.min.y > 0.5f) c = (Color){150, 140, 125, 255};       // toit de tunnel
        else if (b.max.y <= 2.1f && size.x <= 4.0f && size.z <= 4.0f)
            c = (Color){156, 118, 72, 255};                        // caisse
        else if (b.max.y <= 1.7f) c = (Color){196, 172, 128, 255}; // plateau
        Box3(MatrixIdentity(), center, (Vector3){0, 0, 0}, size, c);
        DrawCubeWiresV(center, size, (Color){120, 100, 66, 255});
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
    DrawCircle3D((Vector3){0, 0.02f, -51}, 8, (Vector3){1, 0, 0}, 90,
                 Fade(TeamColor(TEAM_RED), 0.4f));
    DrawCircle3D((Vector3){0, 0.02f, 51}, 8, (Vector3){1, 0, 0}, 90,
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

        if (sp->is_bot) DrawZombieChick(p, display_yaw[i], &anim[i], col);
        else            DrawSoldier(p, display_yaw[i], &anim[i], col,
                                    sp->weapon[sp->slot < WEAPON_SLOTS ? sp->slot : 0]);

        // Ombre "pastille" du premier jeu : elle se resserre quand il saute
        // et s'étale quand il est couché.
        DrawBlobShadow(p, 0.45f * (1.0f - anim[i].air * 0.3f) + anim[i].death * 0.18f);
    }

    for (int i = 0; i < TRACER_MAX; i++) {
        if (tracers[i].active)
            DrawLine3D(tracers[i].from, tracers[i].to, (Color){255, 230, 120, 255});
    }
}

// Passe de rendu du "view model" : l'arme est dessinée APRÈS la scène,
// avec une caméra fixe à l'origine et SANS test de profondeur : elle
// passe toujours devant le décor et ne rentre jamais dans les murs.
// Le modèle vient de weapons.c (le même que celui que tiennent les autres
// soldats) ; ici on l'anime : recul, rechargement (l'arme bascule et le
// chargeur sort), changement d'arme (elle descend puis remonte),
// réarmement (verrou, pompe, glissière).
static void DrawViewModel(void)
{
    if (!my_alive) return;   // pas d'arme à l'écran quand on est mort
    if (scoped) return;      // dans la lunette : l'arme n'est plus à l'écran

    Camera3D vc = {0};
    vc.position = (Vector3){0, 0, 0};
    vc.target = (Vector3){0, 0, -1};   // regarde vers -Z (convention espace vue)
    vc.up = (Vector3){0, 1, 0};
    // FOV propre à l'arme, indépendant de celui du monde : l'arme garde
    // une taille agréable et grossit légèrement en visant
    vc.fovy = Lerp(55.0f, 45.0f, ads);
    vc.projection = CAMERA_PERSPECTIVE;

    // Animations : rechargement (0 -> 1 -> 0 : elle bascule vers le bas,
    // le chargeur tombe puis revient), changement (descend hors champ)
    float rl = (reload_t > 0.0f && reload_total > 0.0f) ? 1.0f - reload_t / (reload_total + 0.05f) : 0.0f;
    float rl_k = sinf(rl * PI);                 // 0 au début, 1 au milieu, 0 à la fin
    float sw = (switch_t > 0.0f) ? switch_t / (WEAPON_SWITCH_TIME + 0.05f) : 0.0f;
    float lower = fmaxf(rl_k * 0.08f, sw * 0.35f);
    Matrix anim = MatrixMultiply(MatrixRotateXYZ((Vector3){-rl_k * 0.5f - sw * 0.9f, rl_k * 0.25f, -rl_k * 0.3f}),
                                 MatrixTranslate(rl_k * 0.02f, -lower, rl_k * 0.03f));
    WeaponPose pose = {0};
    pose.muzzle = (muzzle_t > 0) ? 0.8f + Randf(0, 0.6f) : 0.0f;
    pose.mag_out = (rl > 0.15f && rl < 0.85f) ? sinf((rl - 0.15f) / 0.7f * PI) : 0.0f;
    pose.bolt = sinf(bolt_t * PI);

    BeginMode3D(vc);
        rlDisableDepthTest();          // l'arme s'affiche par-dessus tout
        BatchBegin();
        DrawWeaponModel(MatrixMultiply(anim, WeaponViewMatrix()), my_weapon[my_slot], &pose);
        BatchEnd();
        rlEnableDepthTest();
    EndMode3D();
}

// La lunette du FR-F2 : tout l'écran noir sauf un cercle, un réticule fin
static void DrawScopeOverlay(void)
{
    if (!scoped || !my_alive) return;
    int cx = UIW / 2, cy = UIH / 2;
    float r = UIH * 0.42f;
    DrawRing((Vector2){(float)cx, (float)cy}, r, UIH * 1.2f, 0, 360, 64, BLACK);
    DrawRing((Vector2){(float)cx, (float)cy}, r - 6, r, 0, 360, 64, (Color){30, 30, 30, 255});
    DrawLine(cx - (int)r, cy, cx + (int)r, cy, Fade(BLACK, 0.9f));
    DrawLine(cx, cy - (int)r, cx, cy + (int)r, Fade(BLACK, 0.9f));
    DrawCircle(cx, cy, 2, RED);
}

// Aperçu 3D d'une arme dans un menu : elle tourne lentement dans un cadre
// de l'écran. Le cadre a le ratio de l'écran, pour pouvoir simplement
// restreindre la zone de rendu (rlViewport) sans déformer la projection.
static void DrawWeaponPreview(void)
{
    Rectangle rect;
    int weapon;
    if (!MenuWeaponPreview(&rect, &weapon)) return;
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int px = (int)(rect.x * ui_scale), pw = (int)(rect.width * ui_scale);
    int ph = (int)(rect.height * ui_scale);
    int py = sh - (int)((rect.y + rect.height) * ui_scale);   // OpenGL : origine en bas
    if (pw < 8 || ph < 8) return;
    rlViewport(px, py, pw, ph);
    Camera3D cam = {0};
    cam.position = (Vector3){0.0f, 0.35f, 0.9f};
    cam.target = (Vector3){0, 0, 0};
    cam.up = (Vector3){0, 1, 0};
    cam.fovy = 40.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    BeginMode3D(cam);
        rlDisableDepthTest();
        BatchBegin();
        // Posée à plat, canon vers la gauche, tournant doucement
        Matrix m = MatrixMultiply(MatrixRotateY((float)GetTime() * 0.8f), MatrixTranslate(0, 0.02f, 0));
        Vector3 mz = WeaponMuzzle(weapon);
        m = MatrixMultiply(MatrixTranslate(0, 0, -mz.z * 0.45f), m);   // centrée sur sa longueur
        DrawWeaponModel(m, weapon, NULL);
        BatchEnd();
        rlEnableDepthTest();
    EndMode3D();
    rlViewport(0, 0, sw, sh);
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
    if (!my_alive) return;
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

    // --- Manches gagnées, numéro de manche et chrono, en haut au centre ---
    snprintf(buf, sizeof(buf), "%d", snap.score[TEAM_RED]);
    DrawRectangle(UIW / 2 - 110, 12, 70, 34, Fade(TeamColor(TEAM_RED), 0.8f));
    DrawText(buf, UIW / 2 - 110 + 35 - MeasureText(buf, 24) / 2, 18, 24, WHITE);
    snprintf(buf, sizeof(buf), "%d", snap.score[TEAM_BLUE]);
    DrawRectangle(UIW / 2 + 40, 12, 70, 34, Fade(TeamColor(TEAM_BLUE), 0.8f));
    DrawText(buf, UIW / 2 + 40 + 35 - MeasureText(buf, 24) / 2, 18, 24, WHITE);
    {
        int t = (int)(snap.round_time + 0.999f);
        if (snap.round_phase == ROUND_PLAY) snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
        else snprintf(buf, sizeof(buf), "--:--");
        DrawRectangle(UIW / 2 - 36, 12, 72, 34, Fade(BLACK, 0.55f));
        DrawText(buf, UIW / 2 - MeasureText(buf, 22) / 2, 19, 22, RAYWHITE);
        snprintf(buf, sizeof(buf), "MANCHE %d", snap.round);
        DrawText(buf, UIW / 2 - MeasureText(buf, 14) / 2, 50, 14, LIGHTGRAY);
        // Survivants : un point par membre vivant sous chaque score
        int ar = 0, ab = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            SnapshotPlayer *sp = &snap.players[i];
            if (!sp->active || !sp->alive) continue;
            if (sp->team == TEAM_RED) ar++; else ab++;
        }
        for (int i = 0; i < ar && i < 8; i++) DrawCircle(UIW / 2 - 104 + i * 9, 54, 3, TeamColor(TEAM_RED));
        for (int i = 0; i < ab && i < 8; i++) DrawCircle(UIW / 2 + 104 - i * 9, 54, 3, TeamColor(TEAM_BLUE));
    }

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

    // --- L'arme et ses munitions, en bas au centre ---
    if (me && me->active && me->alive) {
        const WeaponStats *w = CurWeapon();
        int cx = UIW / 2 + 120;
        DrawText(w->name, cx - MeasureText(w->name, 18) / 2, UIH - 88, 18, LIGHTGRAY);
        if (w->mag > 0) {
            snprintf(buf, sizeof(buf), "%d", my_mag[my_slot]);
            Color c = (my_mag[my_slot] == 0) ? RED : (my_mag[my_slot] <= w->mag / 4 ? ORANGE : RAYWHITE);
            int wmag = MeasureText(buf, 40);
            DrawText(buf, cx - 8 - wmag, UIH - 62, 40, c);
            DrawText("|", cx - 2, UIH - 60, 36, GRAY);
            snprintf(buf, sizeof(buf), "%d", my_reserve[my_slot]);
            DrawText(buf, cx + 14, UIH - 52, 24, LIGHTGRAY);
        }
        if (reload_t > 0.0f) {
            // Barre de rechargement
            float k = 1.0f - reload_t / (reload_total + 0.05f);
            DrawRectangle(cx - 70, UIH - 20, 140, 6, Fade(BLACK, 0.5f));
            DrawRectangle(cx - 70, UIH - 20, (int)(140 * k), 6, (Color){255, 200, 90, 255});
            DrawText("rechargement", cx - MeasureText("rechargement", 14) / 2, UIH - 36, 14, LIGHTGRAY);
        } else if (w->mag > 0 && my_mag[my_slot] == 0) {
            const char *m = my_reserve[my_slot] > 0 ? "R : recharger" : "plus de munitions";
            DrawText(m, cx - MeasureText(m, 14) / 2, UIH - 36, 14, RED);
        }
        // L'autre arme, en petit
        int other = (my_slot == SLOT_PRIMARY) ? SLOT_SECONDARY : SLOT_PRIMARY;
        snprintf(buf, sizeof(buf), "%d  %s  %d|%d", other + 1, WEAPONS[my_weapon[other]].name,
                 my_mag[other], my_reserve[other]);
        DrawText(buf, cx + 120, UIH - 52, 14, GRAY);
    }
    if (weapon_msg_t > 0.0f)
        DrawText(weapon_msg, UIW / 2 - MeasureText(weapon_msg, 20) / 2, 90, 20,
                 Fade((Color){255, 200, 90, 255}, weapon_msg_t > 1.0f ? 1.0f : weapon_msg_t));

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

    // --- Éliminé : on attend la fin de la manche (pas de respawn) ---
    if (me && me->active && !me->alive && !snap.match_over && snap.round_phase != ROUND_END) {
        DrawRectangle(0, 0, UIW, UIH, Fade(BLACK, 0.5f));
        const char *msg = "Éliminé : en attente de la prochaine manche";
        DrawText(msg, UIW / 2 - MeasureText(msg, 30) / 2, UIH / 2 - 15, 30, RED);
    }

    // --- Fin de manche, ou attente d'une équipe adverse ---
    if (!snap.match_over && snap.round_phase == ROUND_END) {
        DrawRectangle(0, 0, UIW, UIH, Fade(BLACK, 0.45f));
        Color c = RAYWHITE;
        if (snap.round_winner < TEAM_COUNT) {
            snprintf(buf, sizeof(buf), "MANCHE %d : %s L'EMPORTE", snap.round,
                     snap.round_winner == TEAM_RED ? "ROUGE" : "BLEU");
            c = TeamColor(snap.round_winner);
        } else {
            snprintf(buf, sizeof(buf), "MANCHE %d : NULLE", snap.round);
        }
        DrawText(buf, UIW / 2 - MeasureText(buf, 36) / 2, UIH / 2 - 30, 36, c);
        snprintf(buf, sizeof(buf), "manche suivante dans %d s", (int)(snap.round_time + 0.999f));
        DrawText(buf, UIW / 2 - MeasureText(buf, 20) / 2, UIH / 2 + 20, 20, LIGHTGRAY);
    } else if (!snap.match_over && snap.round_phase == ROUND_WAIT) {
        const char *msg = "En attente d'une équipe adverse...";
        DrawText(msg, UIW / 2 - MeasureText(msg, 24) / 2, UIH / 2 - 12, 24, LIGHTGRAY);
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
    if (last_server_msg >= 0.0 && GetTime() - last_server_msg > 2.0) {
        const char *msg = "Connexion au serveur perdue...";
        DrawText(msg, UIW / 2 - MeasureText(msg, 20) / 2, 60, 20, ORANGE);
    }
}

//----------------------------------------------------------------------------------
// Menus
//----------------------------------------------------------------------------------
// Applique ce que les options changent hors du jeu lui-même
static void ApplyOptions(void)
{
    TouchSetSensitivity(options.touch_sens);
#if !MOBILE_UI
    if (options.fullscreen != IsWindowFullscreen()) ToggleFullscreen();
#endif
}

// Une image d'un écran de menu, de la lecture du pointeur au dessin :
// le fond est fourni par l'appelant (déjà dessiné), on ajoute l'écran
// courant par-dessus, en unités d'interface, puis l'aperçu 3D s'il y en a.
static MenuAction MenuDrawFrame(MenuScreen *screen, bool in_game, const char *status)
{
    UiBegin(ui_scale);
    MenuSetScreen(ui_w, ui_h, MOBILE_UI);
    BeginMode2D(UiCamera());
    MenuAction a = MenuFrame(screen, in_game, status);
    EndMode2D();
    DrawWeaponPreview();
    return a;
}

// Écran d'accueil : serveur, pseudo, armes, options. Boucle jusqu'à la
// connexion (vrai) ou l'abandon (faux). Sur mobile, c'est le seul moyen
// d'entrer l'adresse ; sur PC, il remplace la ligne de commande.
static bool MainMenuLoop(void)
{
    const Color BG = {18, 20, 26, 255};
    MenuScreen screen = MENU_MAIN;
    char status[96] = "";
#if defined(PLATFORM_ANDROID)
    double last_log = 0.0;
#endif

    while (!WindowShouldClose()) {
        UiUpdate();
#if defined(PLATFORM_ANDROID)
        // Témoin pour logcat, une fois par seconde : l'état BRUT des entrées
        // et la taille que LE JEU voit (le test en CI s'y cale).
        if (GetTime() - last_log >= 1.0) {
            last_log = GetTime();
            TraceLog(LOG_INFO, "TACTILE: etat touches=%d souris=%d ecran=%dx%d t=%.0f",
                     GetTouchPointCount(), IsMouseButtonDown(MOUSE_BUTTON_LEFT) ? 1 : 0,
                     GetScreenWidth(), GetScreenHeight(), GetTime());
        }
#endif
        BeginDrawing();
        ClearBackground(BG);
        MenuAction a = MenuDrawFrame(&screen, false, status);
        if (options.show_fps) DrawFPS(GetScreenWidth() - 90, GetScreenHeight() - 30);
        EndDrawing();
#if defined(PLATFORM_ANDROID)
        if (UiReleased()) {
            Vector2 u = UiPointer();
            TraceLog(LOG_INFO, "TACTILE: appui relache en (%.0f, %.0f) px -> (%.0f, %.0f) unites",
                     u.x * ui_scale, u.y * ui_scale, u.x, u.y);
        }
#endif
        switch (a) {
        case MA_QUIT:
            return false;
        case MA_OPTIONS_CHANGED:
        case MA_LOADOUT:
            OptionsSave();
            ApplyOptions();
            if (a == MA_LOADOUT) screen = MENU_MAIN;
            break;
        case MA_PLAY: {
            const char *ip = options.server[0] ? options.server : "127.0.0.1";
            const char *name = options.name[0] ? options.name : "Joueur";
            // NetConnect bloque jusqu'à 5 s (10 tentatives) : on affiche
            // d'abord une image, sinon l'écran semble figé.
            BeginDrawing();
            ClearBackground(BG);
            BeginMode2D(UiCamera());
            UiTextCentered(TextFormat("Connexion à %s...", ip), ui_w / 2, 320, 36, RAYWHITE);
            EndMode2D();
            EndDrawing();
            if (NetConnect(ip, name)) { OptionsSave(); return true; }
            snprintf(status, sizeof(status), "Serveur injoignable : %s", ip);
            break;
        }
        default:
            break;
        }
    }
    return false;
}

// Ouvre ou ferme un menu en jeu. Sur PC la souris est rendue au menu et
// reprise par la visée ; sur mobile les doigts posés sont oubliés pour
// que le joystick ne reste pas « tenu ».
static void SetMenu(MenuScreen m)
{
    if (m == menu) return;
    menu = m;
#if !MOBILE_UI
    if (menu == MENU_NONE) DisableCursor(); else EnableCursor();
#endif
    TouchReset();
}

//----------------------------------------------------------------------------------
// Boucle principale
//----------------------------------------------------------------------------------
int main(int argc, char **argv)
{
    OptionsLoad();
    // Ligne de commande (PC) : ./client [ip[:port]] [pseudo] saute l'accueil
    const char *ip_arg = (argc > 1) ? argv[1] : NULL;
    if (argc > 2) snprintf(options.name, sizeof(options.name), "%s", argv[2]);
    const char *name = options.name[0] ? options.name : "Joueur";

#if !MOBILE_UI
    // raylib moins bavard dans la console. PAS sur mobile : logcat y est le
    // seul canal de diagnostic, et c'est sur ses messages INFO (« Program
    // shader loaded ») que le test de fumee en CI verifie que l'affichage
    // s'est initialise.
    SetTraceLogLevel(LOG_WARNING);
#endif

    bool connected = false;
    if (ip_arg) {
        // PC : on se connecte AVANT d'ouvrir la fenêtre, inutile de l'ouvrir
        // si le serveur est injoignable
        printf("EMA Tournament — connexion à %s en tant que \"%s\"\n", ip_arg, name);
        if (!NetConnect(ip_arg, name)) return 1;
        snprintf(options.server, sizeof(options.server), "%s", ip_arg);
        connected = true;
    }

#if defined(PLATFORM_ANDROID)
    // 0 x 0 : raylib prend la taille NATIVE de l'écran. Avec 1280 x 720 il
    // rendrait en 16:9 et agrandirait l'image avec des bandes noires sur les
    // côtés (vu dans l'émulateur : « Upscaling required »). En natif, c'est
    // UiUpdate qui adapte le HUD au ratio réel de l'écran.
    InitWindow(0, 0, "EMA Tournament");
    SetTargetFPS(60);                // 60 Hz : suffisant, et ménage la batterie
#else
    InitWindow(SCREEN_W, SCREEN_H, "EMA Tournament");
    SetTargetFPS(144);
    SetExitKey(KEY_NULL);            // Échap ouvre le menu, il ne quitte plus
#endif
    UiUpdate();
    ApplyOptions();

    if (!connected && !MainMenuLoop()) {
        CloseWindow();
        return 0;
    }
#if !MOBILE_UI
    DisableCursor();   // capture la souris (visée FPS)
#endif
    // L'équipement choisi : le serveur l'applique au spawn
    SendLoadout();

    ModelsInit();   // crée les maillages des personnages (après InitWindow)
    // Un état d'animation par joueur, décalé par son numéro de slot : deux
    // poussins ne clignent pas des yeux au même instant et ne s'ébrouent
    // pas ensemble.
    for (int i = 0; i < MAX_PLAYERS; i++) CharAnimInit(&anim[i], i + 1);
    UiUpdate();
    TouchReset();

    // Position de départ : premier spawn de notre équipe. Le premier
    // snapshot du serveur corrigera si besoin.
    my_sim.pos = TEAM_SPAWNS[my_team][0];
    my_sim.on_ground = true;
    my_yaw = (my_team == TEAM_RED) ? 0.0f : (float)M_PI;   // face au centre
    memset(&snap, 0, sizeof(snap));
    my_weapon[SLOT_PRIMARY] = options.primary;
    my_weapon[SLOT_SECONDARY] = options.secondary;
    last_server_msg = GetTime();

    double last_state_send = 0.0;
    float fov_current = FOV_HIP;
    Camera3D cam = {0};
    char menu_status[96] = "";

    while (!WindowShouldClose() && !quit_requested) {
        // dt plafonné : après un gel (fenêtre déplacée...), pas de pas de
        // physique géant qui ferait traverser les murs
        float dt = GetFrameTime();
        if (dt > 0.05f) dt = 0.05f;

        UiUpdate();                         // l'écran a pu tourner (mobile)
        NetReceive();                       // snapshots + événements

        // --- Menus : Échap (PC) ou le bouton MENU (mobile) ; B : armes ---
        PlayerInput menu_in = {0};
        if (menu == MENU_NONE) {
            TouchUpdate(ui_scale, ui_w, ui_h);  // doigts posés cette image
            TouchApply(&menu_in, ads);          // seul in.menu nous intéresse ici
        }
#if !MOBILE_UI
        if (IsKeyPressed(KEY_ESCAPE)) SetMenu(menu == MENU_NONE ? MENU_PAUSE : MENU_NONE);
        if (IsKeyPressed(KEY_B) && menu == MENU_NONE) SetMenu(MENU_WEAPONS);
#endif
        if (menu_in.menu) SetMenu(MENU_PAUSE);

        if (menu == MENU_NONE && my_alive && !snap.match_over) UpdateLocalPlayer(dt);
        UpdateViewEffects(dt);              // recul, bloom, minuteurs...
        InterpolateOthers(dt);              // lissage des autres joueurs
        UpdatePlayerCamera(&cam, dt, &fov_current);

        // Envoi périodique de nos entrées au serveur (vide = battement de cœur)
        if (GetTime() - last_state_send > 1.0 / CLIENT_SEND_HZ) {
            last_state_send = GetTime();
            NetSendInputs();
        }

        BeginDrawing();
        ClearBackground((Color){135, 170, 200, 255});   // ciel

        BeginMode3D(cam);       // 1) la scène : map + joueurs + traçantes
        BatchBegin();           //    toutes les primitives des personnages et de
        DrawMap();              //    l'enseigne dans UN maillage, dessiné à BatchEnd
        DrawPlayers();
        BatchEnd();
        EndMode3D();

        DrawViewModel();        // 2) l'arme, par-dessus la scène

        // 3) l'interface, en unités d'interface : la caméra 2D la met à
        // l'échelle de l'écran réel (indispensable sur mobile).
        BeginMode2D(UiCamera());
        DrawScopeOverlay();
        DrawNameTags(cam);
        DrawHUD();
        if (menu == MENU_NONE) TouchDraw();   // joystick et boutons (mobile seulement)
        if (options.show_fps) DrawFPS(UIW - 90, UIH - 30);
        EndMode2D();

        // 4) le menu, s'il est ouvert, par-dessus tout
        if (menu != MENU_NONE) {
            MenuScreen before = menu;
            MenuAction a = MenuDrawFrame(&menu, true, menu_status);
            if (menu != before) SetMenu(menu);
            switch (a) {
            case MA_RESUME:  SetMenu(MENU_NONE); break;
            case MA_QUIT:    quit_requested = true; break;
            case MA_LOADOUT:
                OptionsSave();
                SendLoadout();
                SetMenu(MENU_NONE);
                break;
            case MA_OPTIONS_CHANGED:
                OptionsSave();
                ApplyOptions();
                break;
            default: break;
            }
        }
        EndDrawing();
        {
            // Le bouton d'arme du mobile affiche les munitions de l'arme en main
            char lbl[24];
            const WeaponStats *w = CurWeapon();
            if (w->mag > 0) snprintf(lbl, sizeof(lbl), "%d|%d", my_mag[my_slot], my_reserve[my_slot]);
            else snprintf(lbl, sizeof(lbl), "ARME");
            TouchSetAmmoLabel(lbl);
        }
    }

    NetSendDisconnect();
    ModelsClose();     // libère les maillages (avant CloseWindow)
    CloseWindow();
    SocketClose(sock_fd);
    PlatformNetShutdown();
    return 0;
}
