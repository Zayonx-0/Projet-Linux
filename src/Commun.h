// src/Commun.h
#ifndef COMMUN_H
#define COMMUN_H

/* ─────────────────────────────────────────────────────────────────────────────
   Commun.h — Définitions partagées ServeurISY / GroupeISY / ClientISY
   Inclut:
   - utilitaires (die_perror, trimnl)
   - structures SHM ring buffer (si utilisées)
   - constantes protocole (CREATE/JOIN/LIST + MERGE/REDIRECT + BAN)
   ─────────────────────────────────────────────────────────────────────────── */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <ctype.h>

#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>

/* ───────── Tailles ───────── */
#define ORDRE_LEN  8
#define EME_LEN    20
#define TXT_LEN    512

/* SHM ring buffer (si utilisé) */
#define SHM_RING_CAP 256

typedef struct {
    char Ordre[ORDRE_LEN];     // ex: "MES"
    char Emetteur[EME_LEN];    // ex: "GROUPE"
    char Texte[TXT_LEN];       // payload affiché
} ISYMsg;

typedef struct {
    volatile uint32_t widx;    // index écriture (monotone)
    volatile int closed;       // 1 => affichage doit quitter
    ISYMsg ring[SHM_RING_CAP];
} ISYRing;

/* ───────── Protocole client <-> serveur (ServeurISY UDP) ─────────
   Requêtes:
     "LIST"
     "CREATE <group>"
     "JOIN <group> <user> <cip> <cport>"      (cip/cport ignorés côté groupe, NAT OK)
     "MERGE <user> <tokenA> <groupA> <tokenB> <groupB>"   (à implémenter)
   Réponses:
     "OK <group> <port>"  ou "ERR <reason>"
*/
#define ISY_CMD_LIST    "LIST"
#define ISY_CMD_CREATE  "CREATE"
#define ISY_CMD_JOIN    "JOIN"
#define ISY_CMD_MERGE   "MERGE"     /* nouvelle feature */

/* ───────── Protocole admin serveur -> groupes (vers GroupeISY UDP local) ─────
   Contrôles existants:
     "CTRL BANNER_SET <txt>"
     "CTRL BANNER_CLR"
     "CTRL IBANNER_SET <txt>"
     "CTRL IBANNER_CLR"
   Nouveaux contrôles:
     "CTRL REDIRECT <newGroup> <newPort> <reason...>"
       -> le groupe B demande à ses clients de basculer vers le groupe A.
*/
#define ISY_CTRL_PREFIX       "CTRL"
#define ISY_CTRL_BANNER_SET   "CTRL BANNER_SET"
#define ISY_CTRL_BANNER_CLR   "CTRL BANNER_CLR"
#define ISY_CTRL_IBANNER_SET  "CTRL IBANNER_SET"
#define ISY_CTRL_IBANNER_CLR  "CTRL IBANNER_CLR"
#define ISY_CTRL_REDIRECT     "CTRL REDIRECT" /* nouvelle feature */

/* ───────── Protocole client <-> groupe (GroupeISY UDP) ─────────
   Messages:
     "MSG <user> <texte>"
   Commandes:
     "CMD LIST"
     "CMD DELETE <user>"       (historique: "ban" léger interne, pas persistant)
   Nouveaux:
     "CMD BAN <adminToken> <user>"
     "CMD UNBAN <adminToken> <user>"
*/
#define ISY_MSG_PREFIX     "MSG"
#define ISY_CMD_PREFIX     "CMD"
#define ISY_CMD_G_LIST     "CMD LIST"
#define ISY_CMD_G_DELETE   "CMD DELETE"
#define ISY_CMD_G_BAN      "CMD BAN"     /* nouvelle feature */
#define ISY_CMD_G_UNBAN    "CMD UNBAN"   /* nouvelle feature */

/* ───────── Token admin / gestionnaire ─────────
   Pour modération + fusion propre:
   - le serveur attribue un token à la création du groupe
   - le client "gestionnaire" le conserve localement
*/
#define ADMIN_TOKEN_LEN 64

/* ───────── Utilitaires ───────── */
static inline void die_perror(const char *msg){
    perror(msg);
    exit(EXIT_FAILURE);
}
static inline void die_msg(const char *msg){
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

/* enlève \r?\n en fin de string */
static inline void trimnl(char *s){
    if(!s) return;
    size_t n = strlen(s);
    while(n>0 && (s[n-1]=='\n' || s[n-1]=='\r')){
        s[n-1]='\0';
        n--;
    }
}

/* safe strlcpy-like (portable) */
static inline void isy_strcpy(char *dst, size_t dsz, const char *src){
    if(!dst || dsz==0) return;
    if(!src){ dst[0]='\0'; return; }
    strncpy(dst, src, dsz-1);
    dst[dsz-1] = '\0';
}

#endif // COMMUN_H
