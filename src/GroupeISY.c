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

#define MAX_MEMBERS 64
#define MAX_BANS    128

typedef struct {
    char user[EME_LEN];
    struct sockaddr_in addr;
    int inuse;
} Member;

typedef struct {
    char user[EME_LEN];
    int inuse;
} BanRec;

static volatile sig_atomic_t running = 1;

static void on_sigint(int signo){
    (void)signo;
    running = 0;
}

static inline void send_txt(int s, const char *txt, const struct sockaddr_in *to){
    (void)sendto(s, txt, strlen(txt), 0, (const struct sockaddr*)to, sizeof *to);
}

static void set_rcv_timeout(int sock, int ms){
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

/* ───────── State ───────── */
static Member members[MAX_MEMBERS];
static BanRec bans[MAX_BANS];
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static int  admin_banner_active = 0;
static char admin_banner[TXT_LEN];

static int  idle_banner_active = 0;
static char idle_banner[TXT_LEN];

static char g_admin_token[ADMIN_TOKEN_LEN] = {0};

static unsigned idle_timeout_sec = 1800;
static time_t   last_activity = 0;

static char     gname_local[32] = {0};
static uint16_t gport_local = 0;

/* ───────── Ban helpers ───────── */
static int ban_is_banned_nolock(const char *user){
    for(int i=0;i<MAX_BANS;i++){
        if(bans[i].inuse && !strcmp(bans[i].user, user)) return 1;
    }
    return 0;
}

static int ban_add_nolock(const char *user){
    if(ban_is_banned_nolock(user)) return 1;
    for(int i=0;i<MAX_BANS;i++){
        if(!bans[i].inuse){
            bans[i].inuse = 1;
            isy_strcpy(bans[i].user, sizeof bans[i].user, user);
            return 1;
        }
    }
    return 0;
}

static int ban_remove_nolock(const char *user){
    for(int i=0;i<MAX_BANS;i++){
        if(bans[i].inuse && !strcmp(bans[i].user, user)){
            bans[i].inuse = 0;
            bans[i].user[0] = '\0';
            return 1;
        }
    }
    return 0;
}

/* ───────── Member helpers ───────── */
static int member_find_nolock(const char *user){
    for(int i=0;i<MAX_MEMBERS;i++){
        if(members[i].inuse && !strcmp(members[i].user, user)) return i;
    }
    return -1;
}

static int member_add_or_update_nolock(const char *user, const struct sockaddr_in *addr){
    int idx = member_find_nolock(user);
    if(idx < 0){
        for(int i=0;i<MAX_MEMBERS;i++){
            if(!members[i].inuse){
                members[i].inuse = 1;
                isy_strcpy(members[i].user, sizeof members[i].user, user);
                members[i].addr = *addr;
                return i;
            }
        }
        return -1;
    }
    members[idx].addr = *addr;
    return idx;
}

static void member_remove_nolock(const char *user){
    int idx = member_find_nolock(user);
    if(idx >= 0){
        members[idx].inuse = 0;
        members[idx].user[0] = '\0';
        memset(&members[idx].addr, 0, sizeof members[idx].addr);
    }
}

/* ───────── Broadcast helpers ───────── */
static void broadcast_to_all_nolock(int s, const char *payload){
    for(int i=0;i<MAX_MEMBERS;i++){
        if(members[i].inuse){
            send_txt(s, payload, &members[i].addr);
        }
    }
}

static void broadcast_sys_nolock(int s, const char *text){
    char out[TXT_LEN + 96];
    snprintf(out, sizeof out, "GROUPE[%s]: Message de [SERVER] : %s", gname_local, text);
    broadcast_to_all_nolock(s, out);
}

/* ───────── Admin token logic ─────────
   IMPORTANT:
   - Si le token admin est vide, on autorise une "initialisation" sur la 1ère commande admin reçue
     (BAN/UNBAN) et on fixe g_admin_token = tok.
   - Si le serveur envoie plus tard CTRL SETTOKEN <tok>, ça overwrites (pratique).
*/
static int ensure_or_check_admin_token_locked(const char *tok){
    if(!tok || !*tok) return 0;

    if(g_admin_token[0] == '\0'){
        // 1ère initialisation
        isy_strcpy(g_admin_token, sizeof g_admin_token, tok);
        return 1;
    }

    return (strcmp(g_admin_token, tok) == 0);
}

/* ───────── Timer Inactivité ───────── */
typedef struct { int sock; } TimerCtx;

static void fmt_hhmmss(time_t t, char *out, size_t n){
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, n, "%H:%M:%S", &tmv);
}

static void *idle_timer_thread(void *arg){
    TimerCtx *ctx = (TimerCtx*)arg;

    for(;;){
        if(!running) break;
        sleep(1);

        if(idle_timeout_sec == 0) continue;

        time_t now = time(NULL);
        time_t since;
        int need_warn = 0, need_clear = 0, do_exit = 0;
        char warn_payload[TXT_LEN + 32];

        pthread_mutex_lock(&mtx);
        since = now - last_activity;

        // warning when remaining <= 50% (simple)
        unsigned warn_threshold = (idle_timeout_sec >= 2 ? idle_timeout_sec / 2 : idle_timeout_sec);

        if(since >= (time_t)idle_timeout_sec){
            if(idle_banner_active) need_clear = 1;
            do_exit = 1;
        } else if(since >= (time_t)warn_threshold){
            if(!idle_banner_active){
                time_t deletion_time = last_activity + (time_t)idle_timeout_sec;
                char hhmmss[16];
                fmt_hhmmss(deletion_time, hhmmss, sizeof hhmmss);

                snprintf(idle_banner, sizeof idle_banner,
                         "Inactivite detectee: le groupe '%s' sera supprime a %s sans activite.",
                         gname_local, hhmmss);

                idle_banner_active = 1;
                snprintf(warn_payload, sizeof warn_payload, "CTRL IBANNER_SET %s", idle_banner);
                need_warn = 1;
            }
        }
        pthread_mutex_unlock(&mtx);

        if(need_warn){
            pthread_mutex_lock(&mtx);
            broadcast_to_all_nolock(ctx->sock, warn_payload);
            pthread_mutex_unlock(&mtx);
        }

        if(need_clear){
            pthread_mutex_lock(&mtx);
            broadcast_to_all_nolock(ctx->sock, "CTRL IBANNER_CLR");
            pthread_mutex_unlock(&mtx);
        }

        if(do_exit){
            pthread_mutex_lock(&mtx);
            broadcast_to_all_nolock(ctx->sock,
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
    if(argc < 3){
        fprintf(stderr, "Usage: %s <groupName> <port> [IDLE_TIMEOUT_SEC]\n", argv[0]);
        return 1;
    }

    const char *gname = argv[1];
    uint16_t gport = (uint16_t)atoi(argv[2]);
    if(argc >= 4){
        idle_timeout_sec = (unsigned)atoi(argv[3]);
    }

    strncpy(gname_local, gname, sizeof gname_local - 1);
    gport_local = gport;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if(s < 0) die_perror("socket");

    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    set_rcv_timeout(s, 300);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(gport);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if(bind(s, (struct sockaddr*)&addr, sizeof addr) < 0)
        die_perror("bind group");

    memset(members, 0, sizeof members);
    memset(bans, 0, sizeof bans);
    admin_banner_active = 0; admin_banner[0] = '\0';
    idle_banner_active  = 0; idle_banner[0] = '\0';
    g_admin_token[0]    = '\0';
    last_activity        = time(NULL);

    fprintf(stderr, "[GroupeISY] '%s' UDP %u (idle=%us)\n",
            gname_local, (unsigned)gport_local, idle_timeout_sec);

    // start idle timer thread
    pthread_t th_timer;
    TimerCtx tctx = {.sock = s};
    if(pthread_create(&th_timer, NULL, idle_timer_thread, &tctx) == 0){
        pthread_detach(th_timer);
    }

    char buf[TXT_LEN + 256];

    while(running){
        struct sockaddr_in cli;
        socklen_t cl = sizeof cli;
        ssize_t n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr*)&cli, &cl);
        if(n < 0){
            if(errno == EINTR) continue;
            if(errno == EAGAIN || errno == EWOULDBLOCK) continue;
            continue;
        }
        buf[n] = '\0';

        /* Mark activity for MSG and CMD */
        if(!strncmp(buf, "MSG ", 4) || !strncmp(buf, "CMD ", 4)){
            pthread_mutex_lock(&mtx);
            last_activity = time(NULL);
            if(idle_banner_active){
                idle_banner_active = 0;
                broadcast_to_all_nolock(s, "CTRL IBANNER_CLR");
            }
            pthread_mutex_unlock(&mtx);
        }

        /* ── CTRL … ─────────────────────────────────────────────── */
        if(!strncmp(buf, "CTRL ", 5)){
            if(!strncmp(buf, "CTRL BANNER_SET ", 16)){
                const char *t = buf + 16;
                pthread_mutex_lock(&mtx);
                isy_strcpy(admin_banner, sizeof admin_banner, t);
                admin_banner_active = 1;
                broadcast_to_all_nolock(s, buf);
                pthread_mutex_unlock(&mtx);
                continue;
            }
            if(!strcmp(buf, "CTRL BANNER_CLR")){
                pthread_mutex_lock(&mtx);
                admin_banner_active = 0;
                admin_banner[0] = '\0';
                broadcast_to_all_nolock(s, buf);
                pthread_mutex_unlock(&mtx);
                continue;
            }
            if(!strncmp(buf, "CTRL IBANNER_SET ", 18)){
                const char *t = buf + 18;
                pthread_mutex_lock(&mtx);
                isy_strcpy(idle_banner, sizeof idle_banner, t);
                idle_banner_active = 1;
                broadcast_to_all_nolock(s, buf);
                pthread_mutex_unlock(&mtx);
                continue;
            }
            if(!strcmp(buf, "CTRL IBANNER_CLR")){
                pthread_mutex_lock(&mtx);
                idle_banner_active = 0;
                idle_banner[0] = '\0';
                broadcast_to_all_nolock(s, buf);
                pthread_mutex_unlock(&mtx);
                continue;
            }

            // IMPORTANT: server can configure admin token
            if(!strncmp(buf, "CTRL SETTOKEN ", 14)){
                const char *t = buf + 14;
                pthread_mutex_lock(&mtx);
                isy_strcpy(g_admin_token, sizeof g_admin_token, t);
                pthread_mutex_unlock(&mtx);
                continue;
            }

            // Fusion: forward to clients then exit
            if(!strncmp(buf, "CTRL REDIRECT ", 14)){
                pthread_mutex_lock(&mtx);
                broadcast_to_all_nolock(s, buf);
                pthread_mutex_unlock(&mtx);
                sleep(1);
                running = 0;
                continue;
            }

            // default: forward
            pthread_mutex_lock(&mtx);
            broadcast_to_all_nolock(s, buf);
            pthread_mutex_unlock(&mtx);
            continue;
        }

        /* ── CMD … ──────────────────────────────────────────────── */
        if(!strncmp(buf, "CMD ", 4)){
            // CMD BAN <token> <user>
            if(!strncmp(buf, "CMD BAN ", 8)){
                char tok[ADMIN_TOKEN_LEN] = {0};
                char victim[EME_LEN] = {0};
                if(sscanf(buf + 8, "%63s %19s", tok, victim) != 2){
                    send_txt(s, "ERR bad_args", &cli);
                    continue;
                }

                pthread_mutex_lock(&mtx);
                int ok = ensure_or_check_admin_token_locked(tok);
                if(!ok){
                    pthread_mutex_unlock(&mtx);
                    send_txt(s, "ERR not_admin", &cli);
                    continue;
                }

                ban_add_nolock(victim);
                member_remove_nolock(victim);
                pthread_mutex_unlock(&mtx);

                pthread_mutex_lock(&mtx);
                broadcast_sys_nolock(s, "Moderation: un membre a ete banni.");
                pthread_mutex_unlock(&mtx);

                send_txt(s, "OK banned", &cli);
                continue;
            }

            // CMD UNBAN <token> <user>
            if(!strncmp(buf, "CMD UNBAN ", 10)){
                char tok[ADMIN_TOKEN_LEN] = {0};
                char victim[EME_LEN] = {0};
                if(sscanf(buf + 10, "%63s %19s", tok, victim) != 2){
                    send_txt(s, "ERR bad_args", &cli);
                    continue;
                }

                pthread_mutex_lock(&mtx);
                int ok = ensure_or_check_admin_token_locked(tok);
                if(!ok){
                    pthread_mutex_unlock(&mtx);
                    send_txt(s, "ERR not_admin", &cli);
                    continue;
                }

                int removed = ban_remove_nolock(victim);
                pthread_mutex_unlock(&mtx);

                if(removed){
                    pthread_mutex_lock(&mtx);
                    broadcast_sys_nolock(s, "Moderation: un membre a ete debanni.");
                    pthread_mutex_unlock(&mtx);
                    send_txt(s, "OK unbanned", &cli);
                } else {
                    send_txt(s, "OK not_banned", &cli);
                }
                continue;
            }

            send_txt(s, "ERR unknown_cmd", &cli);
            continue;
        }

        /* ── MSG <user> <text...> ───────────────────────────────── */
        if(!strncmp(buf, "MSG ", 4)){
            char user[EME_LEN] = {0};
            char *p = buf + 4;
            if(sscanf(p, "%19s", user) != 1) continue;

            char *uend = strchr(p, ' ');
            if(!uend) continue;
            char *text = uend + 1;
            if(!*text) continue;

            pthread_mutex_lock(&mtx);

            // refuse if banned
            if(ban_is_banned_nolock(user)){
                pthread_mutex_unlock(&mtx);
                send_txt(s, "SYS Vous etes banni de ce groupe.", &cli);
                continue;
            }

            int idx = member_add_or_update_nolock(user, &cli);
            if(idx < 0){
                pthread_mutex_unlock(&mtx);
                send_txt(s, "SYS Groupe plein.", &cli);
                continue;
            }

            // on join: push active banners
            if(!strcmp(text, "(joined)")){
                if(admin_banner_active){
                    char ctrl[TXT_LEN + 32];
                    snprintf(ctrl, sizeof ctrl, "CTRL BANNER_SET %s", admin_banner);
                    send_txt(s, ctrl, &members[idx].addr);
                }
                if(idle_banner_active){
                    char ctrl[TXT_LEN + 32];
                    snprintf(ctrl, sizeof ctrl, "CTRL IBANNER_SET %s", idle_banner);
                    send_txt(s, ctrl, &members[idx].addr);
                }
            }

            pthread_mutex_unlock(&mtx);

            // if (left): remove
            if(!strcmp(text, "(left)")){
                pthread_mutex_lock(&mtx);
                member_remove_nolock(user);
                pthread_mutex_unlock(&mtx);
            }

            char out[TXT_LEN + 96];
            snprintf(out, sizeof out, "GROUPE[%s]: Message de %s : %s", gname_local, user, text);

            pthread_mutex_lock(&mtx);
            broadcast_to_all_nolock(s, out);
            pthread_mutex_unlock(&mtx);

            continue;
        }

        /* ── SYS ... ─────────────────────────────────────────────── */
        if(!strncmp(buf, "SYS ", 4)){
            const char *text = buf + 4;
            if(*text){
                pthread_mutex_lock(&mtx);
                broadcast_sys_nolock(s, text);
                pthread_mutex_unlock(&mtx);
            }
            continue;
        }
    }

    close(s);
    fprintf(stderr, "[GroupeISY] '%s' stopped.\n", gname_local);
    return 0;
}
