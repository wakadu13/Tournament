// sound.c — Synthèse et lecture des sons (voir sound.h)
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "raymath.h"

#include "../common/protocol.h"
#include "sound.h"

#define RATE     22050          // échantillons par seconde : largement assez pour des bruits
#define VOICES   6              // lectures simultanées d'un même son
#define MAX_LEN  (RATE * 3)     // 3 s au plus

static Sound sounds[SND_COUNT][VOICES];
static int   next_voice[SND_COUNT];
static bool  ready = false;
static float master = 0.8f;
static Vector3 listener = {0};
static float   listener_yaw = 0.0f;

//----------------------------------------------------------------------------------
// Synthèse
//----------------------------------------------------------------------------------
static float buf[MAX_LEN];
static int   len = 0;

static float Rnd(void) { return (float)rand() / (float)RAND_MAX * 2.0f - 1.0f; }

static void Begin(float seconds)
{
    len = (int)(seconds * RATE);
    if (len > MAX_LEN) len = MAX_LEN;
    memset(buf, 0, (size_t)len * sizeof(float));
}

// Bruit blanc, enveloppe exponentielle (tau = temps de décroissance), à partir de t0
static void Noise(float t0, float tau, float amp)
{
    int i0 = (int)(t0 * RATE);
    for (int i = i0; i < len; i++) {
        float t = (float)(i - i0) / RATE;
        buf[i] += Rnd() * amp * expf(-t / tau);
    }
}

// Sinusoïde de f0 glissant vers f1, enveloppe exponentielle
static void Tone(float t0, float dur, float f0, float f1, float tau, float amp)
{
    int i0 = (int)(t0 * RATE), i1 = (int)((t0 + dur) * RATE);
    if (i1 > len) i1 = len;
    float phase = 0.0f;
    for (int i = i0; i < i1; i++) {
        float t = (float)(i - i0) / RATE;
        float f = f0 + (f1 - f0) * (t / dur);
        phase += 2.0f * PI * f / RATE;
        buf[i] += sinf(phase) * amp * expf(-t / tau);
    }
}

// Passe-bas à un pôle : adoucit (cutoff en Hz)
static void LowPass(float cutoff)
{
    float rc = 1.0f / (2.0f * PI * cutoff);
    float a = (1.0f / RATE) / (rc + 1.0f / RATE);
    float y = 0.0f;
    for (int i = 0; i < len; i++) { y += a * (buf[i] - y); buf[i] = y; }
}

// Saturation douce : un tir doit « claquer »
static void Drive(float gain)
{
    for (int i = 0; i < len; i++) buf[i] = tanhf(buf[i] * gain);
}

// Attaque et extinction en douceur (évite les clics), puis normalisation
static Wave Finish(float peak)
{
    int fade = RATE / 200;   // 5 ms
    for (int i = 0; i < fade && i < len; i++) {
        buf[i] *= (float)i / fade;
        buf[len - 1 - i] *= (float)i / fade;
    }
    float m = 0.0f;
    for (int i = 0; i < len; i++) if (fabsf(buf[i]) > m) m = fabsf(buf[i]);
    float k = (m > 1e-6f) ? peak / m : 0.0f;
    short *data = (short *)MemAlloc((unsigned)len * sizeof(short));
    for (int i = 0; i < len; i++) data[i] = (short)(buf[i] * k * 32000.0f);
    Wave w = {0};
    w.frameCount = (unsigned)len;
    w.sampleRate = RATE;
    w.sampleSize = 16;
    w.channels = 1;
    w.data = data;
    return w;
}

static void Make(int id, Wave w)
{
    sounds[id][0] = LoadSoundFromWave(w);
    for (int v = 1; v < VOICES; v++) sounds[id][v] = LoadSoundAlias(sounds[id][0]);
    UnloadWave(w);
}

// Un coup de feu : un claquement de bruit, un « thump » grave, une queue
static Wave Gunshot(float dur, float crack_tau, float cutoff, float thump_f, float thump_amp, float tail)
{
    Begin(dur);
    Noise(0.0f, crack_tau, 1.0f);
    Noise(0.0f, tail, 0.25f);
    Tone(0.0f, 0.12f, thump_f, thump_f * 0.5f, 0.05f, thump_amp);
    LowPass(cutoff);
    Drive(3.0f);
    return Finish(0.9f);
}

static void Generate(void)
{
    Make(SND_SHOT_RIFLE,   Gunshot(0.30f, 0.030f, 4200, 110, 0.9f, 0.09f));
    Make(SND_SHOT_SMG,     Gunshot(0.20f, 0.020f, 5000,  150, 0.6f, 0.06f));
    Make(SND_SHOT_PISTOL,  Gunshot(0.22f, 0.018f, 5500,  170, 0.7f, 0.05f));
    Make(SND_SHOT_SHOTGUN, Gunshot(0.50f, 0.060f, 2600,   70, 1.3f, 0.16f));
    Make(SND_SHOT_SNIPER,  Gunshot(0.70f, 0.045f, 3000,   60, 1.4f, 0.25f));

    Begin(0.35f);                       // griffe : un souffle qui monte, un claquement de bec
    Noise(0.0f, 0.12f, 0.8f);
    Tone(0.15f, 0.06f, 900, 1400, 0.03f, 0.6f);
    LowPass(2500);
    Make(SND_CLAW, Finish(0.6f));

    Begin(0.25f);                       // chargeur qui sort : deux clics mats
    Noise(0.0f, 0.012f, 1.0f);
    Noise(0.12f, 0.02f, 0.8f);
    Tone(0.12f, 0.08f, 220, 180, 0.04f, 0.5f);
    LowPass(2000);
    Make(SND_RELOAD, Finish(0.5f));

    Begin(0.2f);                        // armement : clic-clac métallique
    Noise(0.0f, 0.01f, 1.0f);
    Tone(0.0f, 0.05f, 1800, 1200, 0.02f, 0.4f);
    Noise(0.09f, 0.015f, 0.9f);
    Tone(0.09f, 0.05f, 1400, 900, 0.02f, 0.4f);
    LowPass(4000);
    Make(SND_SWITCH, Finish(0.5f));

    Begin(0.08f);                       // vide : clic sec
    Noise(0.0f, 0.006f, 1.0f);
    Tone(0.0f, 0.03f, 2500, 1500, 0.01f, 0.5f);
    Make(SND_EMPTY, Finish(0.45f));

    Begin(0.08f);                       // touché : tic aigu
    Tone(0.0f, 0.06f, 2400, 2000, 0.02f, 1.0f);
    Make(SND_HITMARK, Finish(0.4f));

    Begin(0.25f);                       // encaissé : coup sourd
    Tone(0.0f, 0.2f, 120, 60, 0.07f, 1.0f);
    Noise(0.0f, 0.05f, 0.5f);
    LowPass(900);
    Make(SND_HURT, Finish(0.7f));

    Begin(0.09f);                       // pas : bruit mat
    Noise(0.0f, 0.025f, 1.0f);
    LowPass(700);
    Make(SND_STEP, Finish(0.35f));

    Begin(0.2f);                        // réception d'un saut
    Noise(0.0f, 0.05f, 1.0f);
    Tone(0.0f, 0.1f, 90, 50, 0.05f, 0.8f);
    LowPass(600);
    Make(SND_LAND, Finish(0.6f));

    Begin(0.35f);                       // piaillement : deux chirps qui montent, avec vibrato
    Tone(0.0f, 0.12f, 1500, 2600, 0.08f, 0.8f);
    Tone(0.17f, 0.14f, 1700, 2900, 0.09f, 0.8f);
    for (int i = 0; i < len; i++) buf[i] *= 0.8f + 0.2f * sinf((float)i / RATE * 2 * PI * 40);
    Make(SND_CHIRP, Finish(0.45f));

    Begin(0.09f);                       // bip de la bombe
    Tone(0.0f, 0.08f, 1300, 1300, 0.2f, 1.0f);
    Make(SND_BEEP, Finish(0.5f));

    Begin(0.5f);                        // bombe posée : trois notes
    Tone(0.00f, 0.1f, 880, 880, 0.08f, 0.8f);
    Tone(0.15f, 0.1f, 880, 880, 0.08f, 0.8f);
    Tone(0.30f, 0.2f, 1320, 1320, 0.1f, 0.8f);
    Make(SND_PLANT, Finish(0.55f));

    Begin(0.6f);                        // désamorcée : deux notes descendantes puis un souffle
    Tone(0.0f, 0.15f, 1100, 1100, 0.1f, 0.8f);
    Tone(0.2f, 0.3f, 700, 600, 0.15f, 0.8f);
    Noise(0.35f, 0.1f, 0.3f);
    LowPass(3000);
    Make(SND_DEFUSE, Finish(0.55f));

    Begin(2.5f);                        // explosion : grondement long, très grave
    Noise(0.0f, 0.5f, 1.0f);
    Noise(0.0f, 0.08f, 1.5f);
    Tone(0.0f, 1.5f, 55, 30, 0.6f, 1.2f);
    LowPass(500);
    Drive(2.5f);
    Make(SND_EXPLOSION, Finish(1.0f));

    Begin(0.6f);                        // début de manche : deux notes
    Tone(0.0f, 0.2f, 660, 660, 0.15f, 0.7f);
    Tone(0.25f, 0.3f, 990, 990, 0.2f, 0.7f);
    Make(SND_ROUND_START, Finish(0.5f));

    Begin(1.0f);                        // victoire : arpège montant
    Tone(0.0f, 0.2f, 523, 523, 0.15f, 0.7f);
    Tone(0.2f, 0.2f, 659, 659, 0.15f, 0.7f);
    Tone(0.4f, 0.2f, 784, 784, 0.15f, 0.7f);
    Tone(0.6f, 0.4f, 1047, 1047, 0.3f, 0.7f);
    Make(SND_WIN, Finish(0.5f));

    Begin(1.0f);                        // défaite : deux notes qui tombent
    Tone(0.0f, 0.35f, 440, 420, 0.3f, 0.7f);
    Tone(0.4f, 0.6f, 330, 280, 0.4f, 0.7f);
    Make(SND_LOSE, Finish(0.5f));

    Begin(0.04f);                       // clic d'interface
    Tone(0.0f, 0.03f, 1800, 1500, 0.01f, 1.0f);
    Make(SND_CLICK, Finish(0.3f));
}

//----------------------------------------------------------------------------------
// Lecture
//----------------------------------------------------------------------------------
void SoundInit(void)
{
    if (ready) return;
    InitAudioDevice();
    if (!IsAudioDeviceReady()) return;   // pas de son : tout reste muet, sans erreur
    Generate();
    SetMasterVolume(master);
    ready = true;
}

void SoundClose(void)
{
    if (!ready) return;
    for (int id = 0; id < SND_COUNT; id++) {
        for (int v = VOICES - 1; v >= 1; v--) UnloadSoundAlias(sounds[id][v]);
        UnloadSound(sounds[id][0]);
    }
    CloseAudioDevice();
    ready = false;
}

void SoundSetVolume(float m)
{
    master = (m < 0) ? 0 : (m > 1 ? 1 : m);
    if (ready) SetMasterVolume(master);
}

void SoundSetListener(Vector3 pos, float yaw)
{
    listener = pos;
    listener_yaw = yaw;
}

static void PlayVoice(int id, float volume, float pitch, float pan)
{
    if (!ready || id < 0 || id >= SND_COUNT || volume <= 0.001f) return;
    Sound *s = &sounds[id][next_voice[id]];
    next_voice[id] = (next_voice[id] + 1) % VOICES;
    SetSoundVolume(*s, volume > 1.0f ? 1.0f : volume);
    SetSoundPitch(*s, pitch);
    SetSoundPan(*s, pan);
    PlaySound(*s);
}

void SoundPlay(int id, float volume, float pitch)
{
    PlayVoice(id, volume, pitch, 0.5f);
}

void SoundPlayAt(int id, Vector3 pos, float volume, float range)
{
    if (!ready) return;
    Vector3 d = Vector3Subtract(pos, listener);
    float dist = Vector3Length(d);
    if (dist >= range) return;
    // Atténuation : pleine à moins de 3 m, puis en (1 - d/range)^1.5
    float k = (dist < 3.0f) ? 1.0f : powf(1.0f - dist / range, 1.5f);
    // Stéréo : la composante du son sur la droite de l'auditeur. raylib :
    // pan 0 = tout à gauche, 1 = tout à droite... et SetSoundPan est
    // défini à l'envers (0 = droite) : on inverse.
    float rx = cosf(listener_yaw), rz = -sinf(listener_yaw);   // droite de yaw (yaw 0 = +Z)
    float side = (dist > 0.01f) ? (d.x * rx + d.z * rz) / dist : 0.0f;
    float pan = 0.5f - side * 0.35f;
    PlayVoice(id, volume * k, 0.95f + 0.1f * (float)rand() / (float)RAND_MAX, pan);
}

int SoundForWeapon(int weapon)
{
    switch (weapon) {
    case WPN_MP5:     return SND_SHOT_SMG;
    case WPN_GLOCK:
    case WPN_PAMAS:   return SND_SHOT_PISTOL;
    case WPN_BENELLI: return SND_SHOT_SHOTGUN;
    case WPN_FRF2:    return SND_SHOT_SNIPER;
    case WPN_CLAWS:   return SND_CLAW;
    default:          return SND_SHOT_RIFLE;
    }
}
