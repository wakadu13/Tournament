// server/main.c — Serveur dédié TDM (UDP, autoritaire sur vie/scores)
//
// Usage : ./server [port]
//
// Le client simule son propre déplacement (prédiction) et envoie sa
// position ; le serveur reste autoritaire sur les tirs, la vie, les
// kills, les scores et les respawns.

// platform.h d'abord : sockets, horloge monotone et sleep, quel que soit
// le systeme (voir src/common/platform.h).
#include "../common/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/protocol.h"
#include "../common/game.h"

typedef struct {
    bool               active;
    struct sockaddr_in addr;
    char               name[NAME_MAX];
    Team               team;
    bool               is_bot;      // relayé aux clients pour le rendu

    PlayerSim sim;          // position, vitesses, au sol : simulé ICI, par les entrées du client
    float yaw, pitch;       // la vue, relayée telle quelle

    // Suivi des entrées (voir PacketClientInput)
    uint16_t last_seq;      // dernière entrée traitée
    bool     has_seq;       // false tant qu'aucune entrée n'est arrivée (après un spawn)
    // Anti-triche sur le temps : un client ne peut pas simuler plus de temps
    // qu'il n'en passe réellement. Le budget se remplit au rythme de
    // l'horloge du serveur et chaque entrée en consomme dt.
    float  time_budget;
    double budget_clock;

    bool    alive;
    uint8_t spawn_count;    // +1 à chaque respawn (voir protocol.h)
    int   health;
    int   kills, deaths;
    float shoot_cooldown;
    double last_heard;      // pour le timeout

    // Armes (voir WEAPONS dans game.c). Le serveur tient les chargeurs :
    // un client modifié ne tire ni plus vite, ni plus fort, ni sans munitions.
    uint8_t  weapon[WEAPON_SLOTS];  // en main : principale, arme de poing
    uint8_t  want[WEAPON_SLOTS];    // équipement demandé, appliqué au prochain spawn
    int      slot;                  // emplacement actif
    int      mag[WEAPON_SLOTS];     // cartouches dans chaque chargeur
    int      reserve[WEAPON_SLOTS]; // cartouches en réserve
    float    reload_t;              // rechargement en cours (s restantes)
    float    switch_t;              // changement d'arme en cours (s restantes)
    uint16_t last_shot_seq;         // dernier tir reçu, accepté ou non
} Player;

static Player   players[MAX_PLAYERS];

// Historique des positions, un cran par tick (~1 s) : pour valider un tir
// à l'instant que le tireur VOYAIT, pas à l'instant où le paquet arrive.
typedef struct {
    uint32_t tick;
    V3       pos[MAX_PLAYERS];
    bool     alive[MAX_PLAYERS];
} HistoryEntry;
static HistoryEntry history[HISTORY_TICKS];
static bool PositionAt(int id, float view_time, V3 *out);   // défini plus bas, utilisé par HandleShoot
static int      team_score[TEAM_COUNT];
static bool     match_over = false;
static int      winner = 0;
static uint32_t tick = 0;

// --- Manches à élimination ---
static int        round_no = 0;
static RoundPhase round_phase = ROUND_WAIT;
static float      round_timer = 0.0f;
static int        round_winner = TEAM_COUNT;   // en ROUND_END ; TEAM_COUNT = nulle
static socket_t sock_fd = SOCKET_NONE;

// Horloge monotone de la boucle de simulation (voir platform.h)
static double Now(void) { return PlatformNow(); }

static bool SameAddr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void SendTo(const struct sockaddr_in *addr, const void *data, size_t len)
{
    sendto(sock_fd, data, len, 0, (const struct sockaddr *)addr, sizeof(*addr));
}

static void Broadcast(const void *data, size_t len)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].active) SendTo(&players[i].addr, data, len);
    }
}

static void BroadcastEvent(EventType ev, int a, int b, int value)
{
    PacketEvent p = {PKT_SV_EVENT, (uint8_t)ev, (uint8_t)a, (uint8_t)b, (int16_t)value};
    Broadcast(&p, sizeof(p));
}

// Remplit les chargeurs et les réserves de l'équipement en main
static void GiveAmmo(Player *p)
{
    for (int k = 0; k < WEAPON_SLOTS; k++) {
        const WeaponStats *w = &WEAPONS[p->weapon[k]];
        p->mag[k] = w->mag;
        p->reserve[k] = w->reserve;
    }
    p->slot = SLOT_PRIMARY;
    p->reload_t = 0.0f;
    p->switch_t = 0.0f;
}

static void SpawnPlayer(Player *p)
{
    int idx = rand() % SPAWNS_PER_TEAM;
    memset(&p->sim, 0, sizeof(p->sim));
    p->sim.pos = TEAM_SPAWNS[p->team][idx];
    p->sim.on_ground = true;
    p->has_seq = false;     // les entrées d'avant le spawn sont périmées (spawn_count)
    p->time_budget = 0.1f;
    p->budget_clock = Now();
    p->yaw = (p->team == TEAM_RED) ? 0.0f : (float)M_PI; // face au centre
    p->pitch = 0.0f;
    p->health = PLAYER_MAX_HEALTH;
    p->alive = true;
    p->spawn_count++;       // invalide les entrées d'avant la mort
    p->shoot_cooldown = 0.0f;
    // L'équipement choisi pendant la manche précédente entre en jeu ici
    memcpy(p->weapon, p->want, sizeof(p->weapon));
    GiveAmmo(p);
}

static int CountTeam(Team t)
{
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].active && players[i].team == t) n++;
    }
    return n;
}

static int CountAlive(Team t)
{
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (players[i].active && players[i].alive && players[i].team == t) n++;
    return n;
}

// Nouvelle manche : tout le monde revit à son spawn. SpawnPlayer incrémente
// spawn_count, ce qui fait repartir la prédiction du client de zéro.
static void StartRound(void)
{
    round_no++;
    round_phase = ROUND_PLAY;
    round_timer = ROUND_TIME;
    round_winner = TEAM_COUNT;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!players[i].active) continue;
        SpawnPlayer(&players[i]);
        BroadcastEvent(EV_RESPAWN, i, 0, 0);
    }
    BroadcastEvent(EV_ROUND_START, (uint8_t)round_no, 0, 0);
    printf("[~] Manche %d : %d ROUGE contre %d BLEU\n", round_no,
           CountTeam(TEAM_RED), CountTeam(TEAM_BLUE));
}

// Fin de manche : un point à l'équipe gagnante (aucun si nulle), pause,
// et le match se termine à ROUNDS_TO_WIN.
static void EndRound(int win)
{
    round_phase = ROUND_END;
    round_timer = ROUND_END_DELAY;
    round_winner = win;
    if (win < TEAM_COUNT) team_score[win]++;
    BroadcastEvent(EV_ROUND_END, (uint8_t)win, (uint8_t)round_no, 0);
    if (win < TEAM_COUNT)
        printf("[~] Manche %d : %s l'emporte (%d - %d)\n", round_no,
               win == TEAM_RED ? "ROUGE" : "BLEU", team_score[TEAM_RED], team_score[TEAM_BLUE]);
    else
        printf("[~] Manche %d : nulle (%d - %d)\n", round_no,
               team_score[TEAM_RED], team_score[TEAM_BLUE]);

    if (win < TEAM_COUNT && team_score[win] >= ROUNDS_TO_WIN) {
        match_over = true;
        winner = win;
        BroadcastEvent(EV_MATCH_OVER, (uint8_t)winner, 0, 0);
        printf("[*] MATCH TERMINÉ — victoire équipe %s\n",
               winner == TEAM_RED ? "ROUGE" : "BLEUE");
    }
}

// La machine à états des manches, appelée à chaque tick.
//   WAIT -> PLAY dès que les deux équipes ont au moins un membre ;
//   PLAY -> END quand une équipe n'a plus de survivant (les deux en même
//           temps : nulle), ou au temps (le plus de survivants l'emporte) ;
//   END  -> PLAY après la pause ; une équipe vide renvoie en WAIT.
static void UpdateRound(float dt)
{
    if (match_over) return;
    bool both = CountTeam(TEAM_RED) > 0 && CountTeam(TEAM_BLUE) > 0;
    switch (round_phase) {
    case ROUND_WAIT:
        if (both) StartRound();
        break;
    case ROUND_PLAY: {
        if (!both) {
            round_phase = ROUND_WAIT;
            printf("[~] Manche %d annulée : une équipe est vide\n", round_no);
            break;
        }
        round_timer -= dt;
        int ar = CountAlive(TEAM_RED), ab = CountAlive(TEAM_BLUE);
        if (ar == 0 && ab == 0)      EndRound(TEAM_COUNT);
        else if (ar == 0)            EndRound(TEAM_BLUE);
        else if (ab == 0)            EndRound(TEAM_RED);
        else if (round_timer <= 0.0f)
            EndRound(ar > ab ? TEAM_RED : (ab > ar ? TEAM_BLUE : TEAM_COUNT));
        break;
    }
    case ROUND_END:
        round_timer -= dt;
        if (round_timer <= 0.0f) {
            if (both) StartRound();
            else round_phase = ROUND_WAIT;
        }
        break;
    }
}

static void HandleConnect(const PacketConnect *pkt, const struct sockaddr_in *from)
{
    // Déjà connecté depuis cette adresse ? On renvoie l'accept (paquet perdu).
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].active && SameAddr(&players[i].addr, from)) {
            PacketAccept acc = {PKT_SV_ACCEPT, (uint8_t)i, (uint8_t)players[i].team};
            SendTo(from, &acc, sizeof(acc));
            return;
        }
    }

    if (pkt->version != PROTO_VERSION) {
        PacketReject rej = {PKT_SV_REJECT, "version incompatible"};
        SendTo(from, &rej, sizeof(rej));
        return;
    }

    int slot = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!players[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        PacketReject rej = {PKT_SV_REJECT, "serveur plein"};
        SendTo(from, &rej, sizeof(rej));
        return;
    }

    // Auto-équilibrage : on rejoint l'équipe la moins nombreuse
    // (calculé avant d'activer le slot pour ne pas se compter soi-même)
    Team team = (CountTeam(TEAM_RED) <= CountTeam(TEAM_BLUE)) ? TEAM_RED : TEAM_BLUE;

    Player *p = &players[slot];
    memset(p, 0, sizeof(*p));
    p->active = true;
    p->addr = *from;
    memcpy(p->name, pkt->name, NAME_MAX);
    p->name[NAME_MAX - 1] = '\0';
    p->team = team;
    p->is_bot = pkt->is_bot != 0;
    p->last_heard = Now();
    // Équipement de départ : un poussin zombie n'a que ses griffes ; un
    // humain part avec le FAMAS et le Glock, et choisira ensuite (PKT_CL_WEAPON).
    p->want[SLOT_PRIMARY]   = p->is_bot ? WPN_CLAWS : WPN_FAMAS;
    p->want[SLOT_SECONDARY] = p->is_bot ? WPN_CLAWS : WPN_GLOCK;
    SpawnPlayer(p);
    // En cours de manche, on attend la suivante : posé au spawn mais
    // éliminé d'office, comme dans Counter-Strike. Sinon la manche
    // (re)démarre au prochain tick et le fera apparaître vivant.
    if (round_phase == ROUND_PLAY) { p->alive = false; p->health = 0; }

    PacketAccept acc = {PKT_SV_ACCEPT, (uint8_t)slot, (uint8_t)p->team};
    SendTo(from, &acc, sizeof(acc));
    BroadcastEvent(EV_PLAYER_JOIN, slot, 0, 0);
    printf("[+] %s rejoint (slot %d, équipe %s)\n",
           p->name, slot, p->team == TEAM_RED ? "ROUGE" : "BLEUE");
}

static void RemovePlayer(int id, const char *why)
{
    if (!players[id].active) return;
    printf("[-] %s parti (%s)\n", players[id].name, why);
    players[id].active = false;
    BroadcastEvent(EV_PLAYER_LEAVE, id, 0, 0);
}

// Vérifie que le paquet vient bien de l'adresse associée à cet id
static Player *ValidSender(int id, const struct sockaddr_in *from)
{
    if (id < 0 || id >= MAX_PLAYERS) return NULL;
    Player *p = &players[id];
    if (!p->active || !SameAddr(&p->addr, from)) return NULL;
    p->last_heard = Now();
    return p;
}

// Équipement, changement d'arme, rechargement (PKT_CL_WEAPON)
static void HandleWeapon(const PacketWeapon *pkt, const struct sockaddr_in *from)
{
    Player *p = ValidSender(pkt->player_id, from);
    if (!p || p->is_bot) return;      // un zombie garde ses griffes

    switch (pkt->action) {
    case WA_LOADOUT: {
        if (!WeaponValidPrimary(pkt->a) || !WeaponValidSecondary(pkt->b)) return;
        p->want[SLOT_PRIMARY] = pkt->a;
        p->want[SLOT_SECONDARY] = pkt->b;
        // Comme dans Counter-Strike, on « achète » en début de manche : le
        // nouvel équipement arrive tout de suite pendant ROUND_BUY_TIME, ou
        // quand on est mort / entre deux manches. Sinon il attend le
        // prochain spawn.
        bool now = !p->alive || round_phase != ROUND_PLAY
                || (ROUND_TIME - round_timer) < ROUND_BUY_TIME;
        if (now) {
            memcpy(p->weapon, p->want, sizeof(p->weapon));
            GiveAmmo(p);
        }
        PacketEvent ev = {PKT_SV_EVENT, EV_LOADOUT, pkt->player_id, 0, now ? 1 : 0};
        SendTo(from, &ev, sizeof(ev));
        break;
    }
    case WA_SWITCH:
        if (pkt->a >= WEAPON_SLOTS || pkt->a == p->slot || !p->alive) return;
        if (p->switch_t > 0.0f) return;
        p->slot = pkt->a;
        p->switch_t = WEAPON_SWITCH_TIME;
        p->reload_t = 0.0f;           // un rechargement en cours est abandonné
        break;
    case WA_RELOAD: {
        if (!p->alive || p->reload_t > 0.0f || p->switch_t > 0.0f) return;
        const WeaponStats *w = &WEAPONS[p->weapon[p->slot]];
        if (w->mag == 0 || p->mag[p->slot] >= w->mag || p->reserve[p->slot] <= 0) return;
        p->reload_t = w->reload;
        break;
    }
    default:
        break;
    }
}

// Fin de rechargement : on complète le chargeur depuis la réserve
static void FinishReload(Player *p)
{
    const WeaponStats *w = &WEAPONS[p->weapon[p->slot]];
    int need = w->mag - p->mag[p->slot];
    if (need > p->reserve[p->slot]) need = p->reserve[p->slot];
    p->mag[p->slot] += need;
    p->reserve[p->slot] -= need;
}

// Nombre aléatoire dans [-1, 1]
static float Rand11(void) { return (float)rand() / (float)RAND_MAX * 2.0f - 1.0f; }

static void HandleShoot(const PacketShoot *pkt, const struct sockaddr_in *from)
{
    Player *shooter = ValidSender(pkt->player_id, from);
    if (!shooter) return;
    // Accusé de réception du tir, accepté ou non : le client en déduit
    // combien de ses tirs sont encore en route (affichage des munitions).
    shooter->last_shot_seq = pkt->shot_seq;
    if (!shooter->alive || match_over) return;
    if (pkt->slot != shooter->slot) return;     // le client change d'arme : tir périmé
    if (shooter->shoot_cooldown > 0.0f) return;
    if (shooter->reload_t > 0.0f || shooter->switch_t > 0.0f) return;

    const WeaponStats *w = &WEAPONS[shooter->weapon[shooter->slot]];
    if (w->mag > 0) {
        if (shooter->mag[shooter->slot] <= 0) return;   // chargeur vide
        shooter->mag[shooter->slot]--;
    }
    shooter->shoot_cooldown = w->cooldown;

    V3 o = {pkt->ox, pkt->oy, pkt->oz};
    V3 d = {pkt->dx, pkt->dy, pkt->dz};
    float dlen = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (dlen < 0.5f || dlen > 1.5f) return; // direction invalide
    d.x /= dlen; d.y /= dlen; d.z /= dlen;

    // Anti-triche basique : l'origine doit être proche de la tête du joueur
    // (sa position prédite peut avoir quelques dizaines de ms d'avance sur
    // la nôtre : 2 m de tolérance)
    V3 eye = {shooter->sim.pos.x, shooter->sim.pos.y + PLAYER_EYE_HEIGHT, shooter->sim.pos.z};
    float ex = o.x - eye.x, ey = o.y - eye.y, ez = o.z - eye.z;
    if (ex * ex + ey * ey + ez * ez > 4.0f) return;

    // Compensation de lag : le tireur visait ce qu'il VOYAIT, c'est-à-dire
    // les autres joueurs INTERP_DELAY dans le passé, plus son ping. On
    // ramène view_time dans l'historique (pas dans le futur, pas plus vieux
    // que ce qu'on garde) et on teste le tir contre les positions de cet
    // instant-là.
    float now_s = (float)tick / SERVER_TICK_HZ;
    float view_time = pkt->view_time;
    if (!(view_time <= now_s)) view_time = now_s;                 // futur, ou NaN
    float oldest = now_s - (float)(HISTORY_TICKS - 2) / SERVER_TICK_HZ;
    if (view_time < oldest) view_time = oldest;

    // Positions des cibles à cet instant, une fois pour tous les plombs
    V3   at[MAX_PLAYERS];
    bool targetable[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *t = &players[i];
        targetable[i] = t->active && t->alive && i != pkt->player_id && t->team != shooter->team;
        if (targetable[i] && !PositionAt(i, view_time, &at[i])) at[i] = t->sim.pos;
    }

    // Base orthonormée autour du rayon, pour disperser les plombs
    V3 r = {d.z, 0, -d.x};
    float rl = sqrtf(r.x * r.x + r.z * r.z);
    if (rl < 1e-4f) r = (V3){1, 0, 0}; else { r.x /= rl; r.z /= rl; }
    V3 u = {r.y * d.z - r.z * d.y, r.z * d.x - r.x * d.z, r.x * d.y - r.y * d.x};

    int  damage[MAX_PLAYERS];
    bool headshot[MAX_PLAYERS];
    memset(damage, 0, sizeof(damage));
    memset(headshot, 0, sizeof(headshot));

    for (int pellet = 0; pellet < w->pellets; pellet++) {
        V3 dir = d;
        if (w->pellets > 1) {
            // Gerbe du fusil à pompe : chaque plomb dans un cône
            float a = Rand11() * 0.035f, b = Rand11() * 0.035f;
            dir.x += r.x * a + u.x * b; dir.y += r.y * a + u.y * b; dir.z += r.z * a + u.z * b;
            float l = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            dir.x /= l; dir.y /= l; dir.z /= l;
        }
        float wall_t = RaycastMap(o, dir);
        // Joueur le plus proche touché avant un mur, à sa position d'alors
        int   hit_id = -1;
        float hit_t = wall_t;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!targetable[i]) continue;
            float ht;
            if (RayVsBox(o, dir, PlayerBox(at[i]), &ht) && ht > 0.0f && ht < hit_t) {
                hit_t = ht;
                hit_id = i;
            }
        }
        if (hit_id < 0 || hit_t > w->range) continue;   // rien, ou trop loin pour cette arme
        // Tir à la tête : le point d'impact est dans les 30 cm du haut
        float hit_y = o.y + dir.y * hit_t - at[hit_id].y;
        bool head = hit_y >= HEADSHOT_HEIGHT;
        damage[hit_id] += head ? (int)(w->damage * HEADSHOT_MULT) : w->damage;
        if (head) headshot[hit_id] = true;
    }

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (damage[i] <= 0) continue;
        Player *victim = &players[i];
        victim->health -= damage[i];
        BroadcastEvent(EV_HIT, pkt->player_id, i, damage[i]);

        if (victim->health <= 0) {
            // Éliminé pour toute la manche : pas de respawn. C'est UpdateRound
            // qui constatera, au prochain tick, qu'une équipe n'a plus personne.
            victim->alive = false;
            victim->deaths++;
            shooter->kills++;
            BroadcastEvent(EV_KILL, pkt->player_id, i,
                           (int)shooter->weapon[shooter->slot] | (headshot[i] ? 0x100 : 0));
            printf("[*] %s a éliminé %s (%s%s ; survivants : %d ROUGE, %d BLEU)\n",
                   shooter->name, victim->name, w->name, headshot[i] ? ", tête" : "",
                   CountAlive(TEAM_RED), CountAlive(TEAM_BLUE));
        }
    }
}

// Rejoue les entrées d'un client que le serveur n'a pas encore vues, dans
// l'ordre, avec le MÊME code de simulation que lui (PlayerStep). C'est ici
// que le serveur fait autorité sur le mouvement : un client modifié peut
// envoyer n'importe quelles entrées, il ne peut pas envoyer de position.
static void ApplyInputs(Player *p, const PacketClientInput *pkt)
{
    int count = pkt->count;
    if (count < 1 || count > INPUT_BATCH) return;

    // Le budget de temps se remplit au rythme de l'horloge du serveur
    // (plafonné : une rafale après un à-coup réseau reste acceptée)
    double now = Now();
    p->time_budget += (float)(now - p->budget_clock);
    p->budget_clock = now;
    if (p->time_budget > 0.25f) p->time_budget = 0.25f;

    for (int i = 0; i < count; i++) {
        const InputEntry *e = &pkt->entries[i];
        // Déjà traitée ? (différence signée sur 16 bits : gère le bouclage)
        if (p->has_seq && (int16_t)(e->seq - p->last_seq) <= 0) continue;

        float dt = e->dt;
        if (!(dt > 0.0f)) continue;                 // 0, négatif ou NaN
        if (dt > INPUT_MAX_DT) dt = INPUT_MAX_DT;   // une image ne dure pas plus
        if (p->time_budget < dt) break;             // plus de temps à simuler : triche ou gel, on attend
        p->time_budget -= dt;

        PlayerCmd c = CmdFromEntry(e);
        // Le déplacement ne peut pas dépasser 1 (le client normalise ; un
        // client modifié n'irait pas plus vite pour autant)
        float m2 = c.move_x * c.move_x + c.move_y * c.move_y;
        if (m2 > 1.0f) { float inv = 1.0f / sqrtf(m2); c.move_x *= inv; c.move_y *= inv; }

        c.speed_mult = WEAPONS[p->weapon[p->slot]].move_mult;   // arme lourde ou légère
        PlayerStep(&p->sim, &c, dt);
        p->yaw = e->yaw;
        p->pitch = e->pitch;
        p->last_seq = e->seq;
        p->has_seq = true;
    }
}

// Position d'un joueur à un instant passé (secondes serveur), interpolée
// entre les deux ticks de l'historique qui l'encadrent. Renvoie false si
// le joueur n'était pas vivant à cet instant ou si l'instant est inconnu.
static bool PositionAt(int id, float view_time, V3 *out)
{
    float ft = view_time * SERVER_TICK_HZ;
    uint32_t t0 = (uint32_t)ft;
    float a = ft - (float)t0;
    const HistoryEntry *h0 = &history[t0 % HISTORY_TICKS];
    const HistoryEntry *h1 = &history[(t0 + 1) % HISTORY_TICKS];
    if (h0->tick != t0) return false;               // trop vieux : déjà écrasé
    if (!h0->alive[id]) return false;
    if (h1->tick != t0 + 1) { *out = h0->pos[id]; return true; }   // tick suivant pas encore écrit
    if (!h1->alive[id]) { *out = h0->pos[id]; return true; }
    out->x = h0->pos[id].x + (h1->pos[id].x - h0->pos[id].x) * a;
    out->y = h0->pos[id].y + (h1->pos[id].y - h0->pos[id].y) * a;
    out->z = h0->pos[id].z + (h1->pos[id].z - h0->pos[id].z) * a;
    return true;
}

static void HandlePacket(const uint8_t *buf, sock_ssize_t len, const struct sockaddr_in *from)
{
    if (len < (sock_ssize_t)sizeof(PacketHeader)) return;

    switch (buf[0]) {
    case PKT_CL_CONNECT:
        if (len >= (sock_ssize_t)sizeof(PacketConnect))
            HandleConnect((const PacketConnect *)buf, from);
        break;

    case PKT_CL_INPUT:
        if (len >= (sock_ssize_t)sizeof(PacketClientInput)) {
            const PacketClientInput *pkt = (const PacketClientInput *)buf;
            Player *p = ValidSender(pkt->player_id, from);
            // spawn_count : on rejette les entrées datant d'avant le dernier
            // respawn (elles feraient bouger le joueur depuis sa mort)
            if (p && p->alive && pkt->spawn_count == p->spawn_count)
                ApplyInputs(p, pkt);
        }
        break;

    case PKT_CL_SHOOT:
        if (len >= (sock_ssize_t)sizeof(PacketShoot))
            HandleShoot((const PacketShoot *)buf, from);
        break;

    case PKT_CL_WEAPON:
        if (len >= (sock_ssize_t)sizeof(PacketWeapon))
            HandleWeapon((const PacketWeapon *)buf, from);
        break;

    case PKT_CL_DISCONNECT:
        if (len >= (sock_ssize_t)sizeof(PacketDisconnect)) {
            const PacketDisconnect *pkt = (const PacketDisconnect *)buf;
            if (ValidSender(pkt->player_id, from))
                RemovePlayer(pkt->player_id, "déconnexion");
        }
        break;
    }
}

static void SendSnapshot(void)
{
    PacketSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.type = PKT_SV_SNAPSHOT;
    snap.tick = tick;
    snap.score[TEAM_RED] = (uint16_t)team_score[TEAM_RED];
    snap.score[TEAM_BLUE] = (uint16_t)team_score[TEAM_BLUE];
    snap.match_over = match_over ? 1 : 0;
    snap.winner = (uint8_t)winner;
    snap.round = (uint8_t)round_no;
    snap.round_phase = (uint8_t)round_phase;
    snap.round_winner = (uint8_t)round_winner;
    snap.round_time = round_timer > 0.0f ? round_timer : 0.0f;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &players[i];
        SnapshotPlayer *s = &snap.players[i];
        s->id = (uint8_t)i;
        s->active = p->active ? 1 : 0;
        if (!p->active) continue;
        s->team = (uint8_t)p->team;
        s->alive = p->alive ? 1 : 0;
        s->is_bot = p->is_bot ? 1 : 0;
        s->spawn_count = p->spawn_count;
        s->health = (int16_t)p->health;
        s->kills = (uint16_t)p->kills;
        s->deaths = (uint16_t)p->deaths;
        s->x = p->sim.pos.x; s->y = p->sim.pos.y; s->z = p->sim.pos.z;
        s->yaw = p->yaw; s->pitch = p->pitch;
        s->vx = p->sim.vel.x; s->vz = p->sim.vel.z; s->vy = p->sim.vel_y;
        s->on_ground = p->sim.on_ground ? 1 : 0;
        s->last_seq = p->last_seq;
        for (int k = 0; k < WEAPON_SLOTS; k++) {
            s->weapon[k] = p->weapon[k];
            s->mag[k] = (uint8_t)(p->mag[k] < 0 ? 0 : (p->mag[k] > 255 ? 255 : p->mag[k]));
            s->reserve[k] = (uint16_t)(p->reserve[k] < 0 ? 0 : p->reserve[k]);
        }
        s->slot = (uint8_t)p->slot;
        s->reload_t = p->reload_t > 0.0f ? p->reload_t : 0.0f;
        s->last_shot_seq = p->last_shot_seq;
        memcpy(s->name, p->name, NAME_MAX);
    }
    Broadcast(&snap, sizeof(snap));
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    srand((unsigned)time(NULL));
    MapInit();
    setvbuf(stdout, NULL, _IOLBF, 0); // logs visibles même redirigés

    if (!PlatformNetInit()) { printf("pile reseau indisponible\n"); return 1; }
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (SocketFailed(sock_fd)) { perror("socket"); return 1; }
    SocketSetNonBlocking(sock_fd);

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons((uint16_t)port);
    if (bind(sock_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind");
        return 1;
    }

    printf("Serveur démarré sur le port %d (tick %d Hz, carte %.0f x %.0f m, %d armes)\n",
           port, SERVER_TICK_HZ, 2 * ARENA_HALF, 2 * ARENA_HALF, WPN_COUNT - 1);

    const double tick_dt = 1.0 / SERVER_TICK_HZ;
    const double snap_dt = 1.0 / SNAPSHOT_HZ;
    double last_tick = Now();
    double last_snap = Now();

    for (;;) {
        // Réception de tous les paquets en attente
        for (;;) {
            uint8_t buf[2048];
            struct sockaddr_in from;
            sock_len_t from_len = sizeof(from);
            sock_ssize_t n = recvfrom(sock_fd, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&from, &from_len);
            if (n <= 0) break;
            HandlePacket(buf, n, &from);
        }

        double now = Now();
        if (now - last_tick >= tick_dt) {
            float dt = (float)(now - last_tick);
            last_tick = now;
            tick++;

            // Historique de ce tick (compensation de lag)
            HistoryEntry *h = &history[tick % HISTORY_TICKS];
            h->tick = tick;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                h->pos[i] = players[i].sim.pos;
                h->alive[i] = players[i].active && players[i].alive;
            }

            for (int i = 0; i < MAX_PLAYERS; i++) {
                Player *p = &players[i];
                if (!p->active) continue;

                if (now - p->last_heard > NET_TIMEOUT_SEC) {
                    RemovePlayer(i, "timeout");
                    continue;
                }
                if (p->shoot_cooldown > 0.0f) p->shoot_cooldown -= dt;
                if (p->switch_t > 0.0f) p->switch_t -= dt;
                if (p->reload_t > 0.0f) {
                    p->reload_t -= dt;
                    if (p->reload_t <= 0.0f) { p->reload_t = 0.0f; FinishReload(p); }
                }
            }
            UpdateRound(dt);
        }

        if (now - last_snap >= snap_dt) {
            last_snap = now;
            SendSnapshot();
        }

        PlatformSleepMs(1);   // ~1 ms, évite de saturer un cœur
    }
    return 0;
}
