#ifndef COMMUN_H
#define COMMUN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <semaphore.h>
#include <pthread.h>

#define ORDRE_LEN 4
#define EME_LEN   20
#define TXT_LEN   100

typedef struct __attribute__((packed)) {
    char Ordre[ORDRE_LEN];
    char Emetteur[EME_LEN];
    char Texte[TXT_LEN];
} ISYMsg;

typedef struct {
    char name[32];
    char moderator[EME_LEN];
    uint16_t port;
    int active;
} GroupInfo;

#define SHM_RING_CAP 128
typedef struct {
    ISYMsg ring[SHM_RING_CAP];
    uint32_t widx;
    uint32_t ridx;
    int closed;
} ISYRing;

static inline void die_perror(const char *msg) { perror(msg); exit(EXIT_FAILURE); }
static inline void trimnl(char *s){ size_t n=strlen(s); if(n && s[n-1]=='\n') s[n-1]='\0'; }

#endif // COMMUN_H
