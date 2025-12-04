// src/GroupeISY.c
#include "Commun.h"
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ────────────────────────────────────────────────────────────────────────────
   GroupeISY  —  tourne sur la machine serveur (IP publique).
   - UDP bind sur <port_groupe> ; reçoit MSG/CMD/CTRL/SYS des clients/serveur.
   - Push auto des bannières actives aux clients qui (re)joignent.
   - Bannière d’inactivité (avertissement + suppression).
   - Préfixe explicite GROUPE[<nom>] dans tous les messages visibles.
   - Message d’expiration avec consigne "quit".
   Arguments:
      argv[1] = nom_groupe
      argv[2] = port_groupe (UDP)
      argv[3] = IDLE_TIMEOUT_SEC (optionnel)
   ─────────────────────────────────────────────────────────────────────────── */

#define MAX_MEMBERS 64

typedef struct {
    char user[EME_LEN];
    struct sockaddr_in addr;
    int banned;
    int inuse;
} Member;

static volatile sig_atomic_t running = 1;

/* ───────── Signals ───────── */
static void on_sigint(int signo){ (void)signo; running = 0; }

/* ───────── Utils ───────── */
static void set_rcv_timeout(int sock, int ms){
    struct timeval tv; tv.tv_sec = ms/1000; tv.tv_usec = (ms%1000)*1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
static inline void send_txt(int s, const char *txt, const struct sockaddr_in *to){
    (void)sendto(s, txt, strlen(txt), 0, (const struct sockaddr*)to, sizeof *to);
}

/* ───────── State ───────── */
static Member members[MAX_MEMBERS];
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

/* Bannière ADMIN (CTRL BANNER_SET/CLR) */
static int  admin_banner_active = 0;
static char admin_banner[TXT_LEN];

/* Bannière INACTIVITÉ (CTRL IBANNER_SET/CLR) */
static int  idle_banner_active = 0;
static char idle_banner[TXT_LEN];

/* Inactivité */
static unsigned idle_timeout_sec = 1800;    // valeur effective injectée par argv[3]
static time_t   last_activity     = 0;

/* Métadonnées */
static char     gname_local[32] = {0};
static uint16_t gport_local     = 0;

/* ───────── Broadcast helpers ───────── */
static void broadcast_to_all(int s, const char *payload){
    for(int i=0;i<MAX_MEMBERS;i++){
        if(members[i].inuse && !members[i].banned){
            send_txt(s, payload, &members[i].addr);
        }
    }
}
static void unicast_to(int s, const char *payload, const struct sockaddr_in *to){
    send_txt(s, payload, to);
}

/* ───────── Thread Timer Inactivité ───────── */
typedef struct { int sock; } TimerCtx;

static void fmt_hhmmss(time_t t, char *out, size_t n){
    struct tm tmv; localtime_r(&t, &tmv);
    strftime(out, n, "%H:%M:%S", &tmv);
}

static void *idle_timer_thread(void *arg){
    TimerCtx *ctx = (TimerCtx*)arg;
    const unsigned warn_threshold = (idle_timeout_sec >= 2 ? idle_timeout_sec/2 : idle_timeout_sec);

    for(;;){
        if(!running) break;
        sleep(1);

        time_t now = time(NULL);
        int do_exit = 0, need_warn = 0, need_clear = 0;
        char warn_payload[TXT_LEN + 32];

        pthread_mutex_lock(&mtx);
        time_t since = now - last_activity;

        if(idle_timeout_sec > 0){
            if(since >= (time_t)idle_timeout_sec){
                if(idle_banner_active) need_clear = 1;
                do_exit = 1;
            }else if(since >= (time_t)warn_threshold){
                if(!idle_banner_active){
                    time_t deletion_time = last_activity + (time_t)idle_timeout_sec;
                    char hhmmss[16]; fmt_hhmmss(deletion_time, hhmmss, sizeof hhmmss);

                    /* Message court pour éviter la troncature dans petits terminaux */
                    snprintf(idle_banner, sizeof idle_banner,
                             "Inactivite detectee: le groupe '%s' sera supprime a %s sans activite.",
                             gname_local, hhmmss);

                    idle_banner_active = 1;
                    snprintf(warn_payload, sizeof warn_payload, "CTRL IBANNER_SET %s", idle_banner);
                    need_warn = 1;
                }
            }
        }
        pthread_mutex_unlock(&mtx);

        if(need_warn){
            pthread_mutex_lock(&mtx);
            broadcast_to_all(ctx->sock, warn_payload);
            pthread_mutex_unlock(&mtx);
        }
        if(need_clear){
            pthread_mutex_lock(&mtx);
            broadcast_to_all(ctx->sock, "CTRL IBANNER_CLR");
            pthread_mutex_unlock(&mtx);
        }
        if(do_exit){
            pthread_mutex_lock(&mtx);
            broadcast_to_all(ctx->sock,
              "SYS Le groupe est supprime pour cause d'inactivite. Tappez \"quit\" pour quitter.");
            pthread_mutex_unlock(&mtx);
            running = 0;
            break;
        }
    }
    return NULL;
}

/* ───────── Main ───────── */
int main(int argc, char **argv){
    if(argc<3){ fprintf(stderr, "Usage: %s <groupName> <port> [IDLE_TIMEOUT_SEC]\n", argv[0]); return 1; }
    const char *gname = argv[1];
    uint16_t gport    = (uint16_t)atoi(argv[2]);
    if(argc>=4){
        unsigned v = (unsigned)atoi(argv[3]);
        idle_timeout_sec = v;
    }

    strncpy(gname_local, gname, sizeof gname_local -1);
    gport_local = gport;

    struct sigaction sa; memset(&sa,0,sizeof sa);
    sa.sa_handler = on_sigint; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    int s = socket(AF_INET, SOCK_DGRAM, 0); if(s<0) die_perror("socket");
    int yes=1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    set_rcv_timeout(s, 500);

    struct sockaddr_in addr={0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(gport);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);        /* écoute sur toutes les interfaces */
    if(bind(s,(struct sockaddr*)&addr,sizeof addr)<0) die_perror("bind group");

    memset(members,0,sizeof members);
    admin_banner_active = 0; admin_banner[0]='\0';
    idle_banner_active  = 0; idle_banner[0]='\0';
    last_activity       = time(NULL);

    fprintf(stdout, "GroupeISY '%s' lance sur %u (IDLE_TIMEOUT_SEC=%u)\n",
            gname, (unsigned)gport, idle_timeout_sec);

    /* Timer d’inactivité */
    pthread_t th_timer;
    TimerCtx tctx = {.sock = s};
    if(pthread_create(&th_timer, NULL, idle_timer_thread, &tctx) != 0){
        perror("pthread_create(idle_timer)");
    }else{
        pthread_detach(th_timer);
    }

    char buf[TXT_LEN + 64];
    while(running){
        struct sockaddr_in cli; socklen_t cl=sizeof cli;
        ssize_t n = recvfrom(s, buf, sizeof buf -1, 0, (struct sockaddr*)&cli, &cl);
        if(n<0){
            if(errno==EINTR) continue;
            if(errno==EAGAIN || errno==EWOULDBLOCK) continue;
            die_perror("recvfrom");
        }
        buf[n]='\0';

        /* ── Trafic utilisateur = activité ───────────── */
        if(!strncmp(buf, "MSG ", 4) || !strncmp(buf, "CMD ", 4)){
            pthread_mutex_lock(&mtx);
            last_activity = time(NULL);
            if(idle_banner_active){
                idle_banner_active = 0;
                broadcast_to_all(s, "CTRL IBANNER_CLR");
            }
            pthread_mutex_unlock(&mtx);
        }

        /* ── MSG <user> <texte...> ─────────────────────────────────── */
        if(!strncmp(buf, "MSG ", 4)){
            char user[EME_LEN]={0}; char *p = buf+4;
            if(sscanf(p, "%19s", user) != 1) continue;

            char *uend = strchr(p, ' ');
            if(!uend) continue;
            char *text = uend + 1;
            if(!*text) continue;

            int idx=-1; int is_new = 0;
            pthread_mutex_lock(&mtx);
            for(int i=0;i<MAX_MEMBERS;i++)
                if(members[i].inuse && !strcmp(members[i].user,user)){ idx=i; break; }
            if(idx<0){
                for(int i=0;i<MAX_MEMBERS;i++) if(!members[i].inuse){
                    idx=i;
                    strncpy(members[i].user,user,EME_LEN-1);
                    members[i].inuse=1;
                    is_new=1;
                    break;
                }
            }
            if(idx>=0){
                members[idx].addr = cli;   /* origin = adresse réelle (NAT ok) */
            }
            int banned = (idx>=0 ? members[idx].banned : 1);
            pthread_mutex_unlock(&mtx);
            if(idx<0 || banned) continue;

            /* Push bannière(s) au client qui (re)joint.
               On repère "(joined)" en texte, + is_new pour first seen. */
            int is_join_msg = !strcmp(text, "(joined)");
            if(is_new || is_join_msg){
                pthread_mutex_lock(&mtx);
                if(admin_banner_active){
                    char ctrl[TXT_LEN + 32];
                    snprintf(ctrl,sizeof ctrl, "CTRL BANNER_SET %s", admin_banner);
                    unicast_to(s, ctrl, &members[idx].addr);
                }
                if(idle_banner_active){
                    char ctrl[TXT_LEN + 32];
                    snprintf(ctrl,sizeof ctrl, "CTRL IBANNER_SET %s", idle_banner);
                    unicast_to(s, ctrl, &members[idx].addr);
                }
                pthread_mutex_unlock(&mtx);
            }

            char out[TXT_LEN + 64];
            snprintf(out,sizeof out,"GROUPE[%s]: Message de %s : %s", gname_local, user, text);
            unicast_to(s, out, &cli);

            pthread_mutex_lock(&mtx);
            for(int i=0;i<MAX_MEMBERS;i++){
                if(!members[i].inuse || members[i].banned) continue;
                if(members[i].addr.sin_addr.s_addr==cli.sin_addr.s_addr &&
                   members[i].addr.sin_port==cli.sin_port) continue;
                send_txt(s, out, &members[i].addr);
            }
            pthread_mutex_unlock(&mtx);

        /* ── CMD LIST / DELETE ─────────────────────────────────────── */
        } else if(!strncmp(buf, "CMD ", 4)){
            if(!strncmp(buf+4, "LIST", 4)){
                char out[1024]={0};
                pthread_mutex_lock(&mtx);
                for(int i=0;i<MAX_MEMBERS;i++) if(members[i].inuse && !members[i].banned){
                    strncat(out, members[i].user, sizeof out - strlen(out) - 1);
                    strncat(out, "\n", sizeof out - strlen(out) - 1);
                }
                pthread_mutex_unlock(&mtx);
                if(out[0]=='\0') strcpy(out,"(aucun)\n");
                unicast_to(s,out,&cli);

            } else if(!strncmp(buf+4, "DELETE ", 7)){
                char who[EME_LEN]={0}; sscanf(buf+11, "%19s", who);
                int done=0;
                pthread_mutex_lock(&mtx);
                for(int i=0;i<MAX_MEMBERS;i++) if(members[i].inuse && !strcmp(members[i].user,who)){
                    members[i].banned=1; done=1;
                }
                pthread_mutex_unlock(&mtx);
                char out[128]; snprintf(out,sizeof out, done?"Action: %s banni\n":"ERR: inconnu\n", who);
                unicast_to(s,out,&cli);
            }

        /* ── SYS <texte...> ────────────────────────────────────────── */
        } else if(!strncmp(buf, "SYS ", 4)){
            const char *text = buf + 4;
            if(*text == '\0') continue;
            char out[TXT_LEN + 64];
            snprintf(out,sizeof out,"GROUPE[%s]: Message de [SERVER] : %s", gname_local, text);
            pthread_mutex_lock(&mtx);
            broadcast_to_all(s,out);
            pthread_mutex_unlock(&mtx);

        /* ── CTRL … (bannières) ───────────────────────────────────── */
        } else if(!strncmp(buf, "CTRL ", 5)){
            if(!strncmp(buf, "CTRL BANNER_SET ", 16)){
                const char *t = buf + 16;
                pthread_mutex_lock(&mtx);
                strncpy(admin_banner, t, TXT_LEN-1); admin_banner[TXT_LEN-1]='\0';
                admin_banner_active = 1;
                broadcast_to_all(s, buf);
                pthread_mutex_unlock(&mtx);
            } else if(!strcmp(buf, "CTRL BANNER_CLR")){
                pthread_mutex_lock(&mtx);
                admin_banner_active = 0; admin_banner[0]='\0';
                broadcast_to_all(s, buf);
                pthread_mutex_unlock(&mtx);
            } else if(!strncmp(buf, "CTRL IBANNER_SET ", 18)){
                const char *t = buf + 18;
                pthread_mutex_lock(&mtx);
                strncpy(idle_banner, t, TXT_LEN-1); idle_banner[TXT_LEN-1]='\0';
                idle_banner_active = 1;
                broadcast_to_all(s, buf);
                pthread_mutex_unlock(&mtx);
            } else if(!strcmp(buf, "CTRL IBANNER_CLR")){
                pthread_mutex_lock(&mtx);
                idle_banner_active = 0; idle_banner[0]='\0';
                broadcast_to_all(s, buf);
                pthread_mutex_unlock(&mtx);
            } else {
                /* relai brut si autre CTRL */
                pthread_mutex_lock(&mtx);
                broadcast_to_all(s, buf);
                pthread_mutex_unlock(&mtx);
            }
        }
    }

    close(s);
    fprintf(stdout, "GroupeISY '%s' arret.\n", gname);
    return 0;
}
