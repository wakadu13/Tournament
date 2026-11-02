/*
 * platform.h — Couche d'adaptation Windows / Linux / macOS / Android.
 *
 * Les trois programmes (serveur, client, bot) parlent UDP avec les mêmes
 * appels. Seuls les noms changent d'un système à l'autre :
 *
 *   | besoin             | POSIX (Linux, macOS, Android) | Windows (Winsock 2) |
 *   |--------------------|-------------------------------|---------------------|
 *   | type d'une socket  | int (descripteur de fichier)  | SOCKET (handle)     |
 *   | fermer             | close()                       | closesocket()       |
 *   | non bloquant       | fcntl(O_NONBLOCK)             | ioctlsocket(FIONBIO)|
 *   | init de la pile    | (rien)                        | WSAStartup()        |
 *   | horloge monotone   | clock_gettime(CLOCK_MONOTONIC)| QueryPerformanceCounter |
 *   | attendre           | usleep()                      | Sleep()             |
 *
 * Tout est en `static inline` : pas de .c à compiler en plus.
 *
 * ATTENTION (Windows) : ce fichier doit être inclus AVANT raylib.h.
 * winsock2.h tire windows.h, qui déclare Rectangle, CloseWindow, ShowCursor
 * et DrawText — les mêmes noms que raylib. NOGDI et NOUSER (définis plus
 * bas) écartent les deux en-têtes fautifs ; l'ordre d'inclusion reste
 * indispensable pour que ces macros soient posées à temps.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

/* --- Macros à poser AVANT tout en-tête système : elles changent ce que
       les en-têtes exposent. --------------------------------------------- */
#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN     /* allège windows.h (exclut le vieux winsock.h) */
    #ifndef NOGDI
        #define NOGDI               /* écarte wingdi.h  : Rectangle */
    #endif
    #ifndef NOUSER
        #define NOUSER              /* écarte winuser.h : CloseWindow, ShowCursor, DrawText */
    #endif
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0601 /* Windows 7 minimum : donne inet_pton */
    #endif
#else
    #ifndef _DEFAULT_SOURCE
        #define _DEFAULT_SOURCE     /* glibc : usleep, M_PI... malgré -std=c11 */
    #endif
    #ifndef _DARWIN_C_SOURCE
        #define _DARWIN_C_SOURCE    /* macOS : idem */
    #endif
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>   /* atoi, pour le port colle a l'hote */
#include <math.h>
#include <string.h>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    /* windows.h définit near et far (reliquat du 16 bits) : ce sont des
       noms de variables parfaitement légitimes ailleurs. */
    #undef near
    #undef far

    typedef SOCKET  socket_t;
    typedef int     sock_len_t;    /* Winsock prend des int, pas des socklen_t */
    typedef int     sock_ssize_t;
    #define SOCKET_NONE INVALID_SOCKET
#else
    #include <arpa/inet.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <netdb.h>          /* getaddrinfo : résolution des noms d'hôtes */
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <time.h>
    #include <unistd.h>

    typedef int        socket_t;
    typedef socklen_t  sock_len_t;
    typedef ssize_t    sock_ssize_t;
    #define SOCKET_NONE (-1)
#endif

/* M_PI ne fait pas partie du C standard : absent en -std=c11 strict. */
#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif

/* --- Pile réseau ------------------------------------------------------- */

/* À appeler une fois avant toute socket. Sans effet (et toujours vrai)
   ailleurs que sous Windows, où Winsock exige WSAStartup. */
static inline bool PlatformNetInit(void)
{
#if defined(_WIN32)
    static bool ready = false;
    if (!ready) {
        WSADATA data;
        ready = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
    }
    return ready;
#else
    return true;
#endif
}

static inline void PlatformNetShutdown(void)
{
#if defined(_WIN32)
    WSACleanup();
#endif
}

static inline void SocketClose(socket_t s)
{
    if (s == SOCKET_NONE) return;
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

/* Socket non bloquante : recvfrom rend la main tout de suite s'il n'y a
   rien à lire, ce qui permet de l'appeler à chaque image du jeu. */
static inline bool SocketSetNonBlocking(socket_t s)
{
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

/* Vrai si la socket n'a pas pu être créée (le test diffère : sous Windows
   SOCKET est un type non signé, « < 0 » ne veut rien dire). */
static inline bool SocketFailed(socket_t s) { return s == SOCKET_NONE; }

/* --- Résolution de l'adresse du serveur -------------------------------- */

/*
 * Convertit « hôte » ou « hôte:port » en adresse UDP prête à l'emploi.
 *
 * On passe par getaddrinfo et NON par inet_pton : inet_pton n'accepte que
 * des adresses numériques, alors que getaddrinfo accepte aussi bien
 * « 192.168.1.10 » qu'un NOM DNS comme « jeu.exemple.fr ». C'est
 * indispensable pour pointer un nom (Cloudflare en DNS seul, DuckDNS...)
 * sur le serveur au lieu de retenir une IP.
 *
 * Le port peut être collé à l'hôte — « jeu.exemple.fr:27015 » — sinon
 * default_port s'applique. Renvoie false si le nom est introuvable.
 */
static inline bool ResolveServerAddr(const char *host_arg, unsigned short default_port,
                                     struct sockaddr_in *out)
{
    char host[256];
    unsigned short port = default_port;

    /* Séparer un éventuel « :port » (le dernier deux-points seulement, pour
       ne pas couper une adresse IPv6 ; on ne gère que l'IPv4 ici, mais
       autant ne pas se tromper de deux-points). */
    size_t n = strlen(host_arg);
    if (n == 0 || n >= sizeof(host)) return false;
    memcpy(host, host_arg, n + 1);
    char *colon = strrchr(host, ':');
    if (colon != NULL && colon[1] != '\0') {
        int p = atoi(colon + 1);
        if (p > 0 && p < 65536) { port = (unsigned short)p; *colon = '\0'; }
    }

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;          /* IPv4 : c'est ce que parle le protocole */
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) return false;

    memset(out, 0, sizeof(*out));
    *out = *(struct sockaddr_in *)(void *)res->ai_addr;
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    freeaddrinfo(res);
    return true;
}

/* --- Temps ------------------------------------------------------------- */

/* Secondes depuis un instant arbitraire, horloge MONOTONE : elle n'est pas
   affectée par un changement d'heure du système, contrairement à time(). */
static inline double PlatformNow(void)
{
#if defined(_WIN32)
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

/* Rend la main au système pendant ms millisecondes (évite de saturer un
   cœur dans les boucles du serveur et du bot). */
static inline void PlatformSleepMs(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

#endif /* PLATFORM_H */
