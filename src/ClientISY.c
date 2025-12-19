// src/ClientISY.c
#include "Commun.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_TOKENS 64

typedef struct {
    char group[32];
    char token[ADMIN_TOKEN_LEN];
    int  has;
} TokenEntry;

typedef struct {
    // server control socket
    int sock_srv;
    struct sockaddr_in srv_addr;

    // group socket (same rx socket)
    int sock_rx;
    struct sockaddr_in grp_addr;

    // session
    int joined;
    char user[EME_LEN];
    char current_group[32];

    // tokens (admin rights)
    TokenEntry tokens[MAX_TOKENS];

    // UI pipe fds
    int ui_in_fd;   // Client -> Affichage (write)
    int ui_out_fd;  // Affichage -> Client (read)
    pid_t ui_pid;
    char fifo_in[256];
    char fifo_out[256];

    // flags from rx thread
    volatile int redirect_pending;
    char redirect_group[32];
    uint16_t redirect_port;
    char redirect_reason[128];

    volatile int group_deleted;
    volatile int stop_rx;

    // n'afficher les messages RX que si on est dans "dialoguer"
    volatile int in_dialogue;

    pthread_t rx_th;
    pthread_mutex_t mtx;
} ClientCtx;

static volatile sig_atomic_t g_running = 1;

/* ───────────────────────── UI helpers (FIFO protocol) ───────────────────────── */

static void ui_send(ClientCtx *c, const char *fmt, ...){
    if(c->ui_in_fd < 0) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t L = strlen(buf);
    if(L == 0) return;
    if(buf[L-1] != '\n') dprintf(c->ui_in_fd, "%s\n", buf);
    else dprintf(c->ui_in_fd, "%s", buf);
}

static void ui_set_header(ClientCtx *c){
    pthread_mutex_lock(&c->mtx);
    int joined = c->joined;
    char user[EME_LEN]; isy_strcpy(user, sizeof user, c->user);
    char grp[32]; grp[0] = '\0';
    if(c->joined) isy_strcpy(grp, sizeof grp, c->current_group);
    pthread_mutex_unlock(&c->mtx);

    ui_send(c, "UI HEADER %d %s %s", joined, user, grp[0]?grp:"-");
}

static void ui_log(ClientCtx *c, const char *fmt, ...){
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ui_send(c, "UI LOG %s", buf);
}

static void ui_help(ClientCtx *c){
    ui_log(c, "=== AIDE CLIENTISY (admin / fusion / moderation) ===");
    ui_log(c, "Devenir ADMIN : cree le groupe via option 0. Si le serveur renvoie un token, il est enregistre.");
    ui_log(c, "Mode dialogue: tape 'cmd' pour entrer en mode commandes, 'msg' pour revenir aux messages.");
    ui_log(c, "Commandes (mode cmd) :");
    ui_log(c, "  help                         -> affiche cette aide");
    ui_log(c, "  admin                        -> liste les tokens");
    ui_log(c, "  settoken <groupe> <token>     -> enregistre un token manuellement");
    ui_log(c, "  ban <pseudo>                  -> bannit un membre");
    ui_log(c, "  unban <pseudo>                -> retire le ban");
    ui_log(c, "  merge <A> <B>                 -> fusionne B vers A (tokens admin A et B requis)");
    ui_log(c, "  msg                          -> retour au mode messages");
    ui_log(c, "  quit                         -> retour au menu principal");
    ui_log(c, "====================================================");
}

static void ui_menu(ClientCtx *c){
    ui_log(c, "Choix des commandes :");
    ui_log(c, "0 Creation de groupe");
    ui_log(c, "1 Rejoindre un groupe");
    ui_log(c, "2 Lister les groupes");
    ui_log(c, "3 Dialoguer sur un groupe");
    ui_log(c, "4 Quitter");
    ui_log(c, "5 Quitter le groupe");
    ui_log(c, "Entrez votre choix :");
}

/* lecture ligne depuis AffichageISY -> ClientISY */
static int ui_readline(ClientCtx *c, char *out, size_t outsz){
    if(c->ui_out_fd < 0) return 0;

    static char buf[4096];
    static size_t len = 0;

    for(;;){
        for(size_t i=0;i<len;i++){
            if(buf[i] == '\n'){
                size_t L = (i < outsz-1) ? i : outsz-1;
                memcpy(out, buf, L);
                out[L] = '\0';
                memmove(buf, buf+i+1, len-(i+1));
                len -= (i+1);
                trimnl(out);
                return 1;
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(c->ui_out_fd, &rfds);
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 300000;

        int r = select(c->ui_out_fd+1, &rfds, NULL, NULL, &tv);
        if(r < 0){
            if(errno == EINTR) continue;
            return 0;
        }
        if(r == 0){
            if(!g_running) return 0;
            continue;
        }

        if(FD_ISSET(c->ui_out_fd, &rfds)){
            ssize_t n = read(c->ui_out_fd, buf+len, sizeof(buf)-len);
            if(n <= 0) return 0;
            len += (size_t)n;
            if(len >= sizeof(buf)) len = 0;
        }
    }
}

/* ───────────────────────── Tokens admin ───────────────────────── */

static TokenEntry* token_find(ClientCtx *c, const char *group){
    for(int i=0;i<MAX_TOKENS;i++){
        if(c->tokens[i].has && !strcmp(c->tokens[i].group, group)) return &c->tokens[i];
    }
    return NULL;
}

static void token_set(ClientCtx *c, const char *group, const char *token){
    pthread_mutex_lock(&c->mtx);
    TokenEntry *e = token_find(c, group);
    if(!e){
        for(int i=0;i<MAX_TOKENS;i++){
            if(!c->tokens[i].has){
                e = &c->tokens[i];
                e->has = 1;
                isy_strcpy(e->group, sizeof e->group, group);
                break;
            }
        }
    }
    if(e) isy_strcpy(e->token, sizeof e->token, token);
    pthread_mutex_unlock(&c->mtx);
}

static const char* token_get(ClientCtx *c, const char *group){
    TokenEntry *e = token_find(c, group);
    return e ? e->token : NULL;
}

static void token_print(ClientCtx *c){
    ui_log(c, "=== TOKENS ADMIN ENREGISTRES ===");
    int any = 0;
    for(int i=0;i<MAX_TOKENS;i++){
        if(c->tokens[i].has){
            ui_log(c, "  - %s : %s", c->tokens[i].group, c->tokens[i].token);
            any = 1;
        }
    }
    if(!any) ui_log(c, "  (aucun token)");
    ui_log(c, "===============================");
}

/* ───────────────────────── Group join/leave ───────────────────────── */

static void group_send_join_hello(ClientCtx *c){
    char hello[128];
    pthread_mutex_lock(&c->mtx);
    snprintf(hello, sizeof hello, "MSG %s %s", c->user, "(joined)");
    pthread_mutex_unlock(&c->mtx);
    (void)sendto(c->sock_rx, hello, strlen(hello), 0, (struct sockaddr*)&c->grp_addr, sizeof c->grp_addr);
}

static void group_send_left(ClientCtx *c){
    char bye[128];
    pthread_mutex_lock(&c->mtx);
    snprintf(bye, sizeof bye, "MSG %s %s", c->user, "(left)");
    pthread_mutex_unlock(&c->mtx);
    (void)sendto(c->sock_rx, bye, strlen(bye), 0, (struct sockaddr*)&c->grp_addr, sizeof c->grp_addr);
}

static void cleanup_joined_state(ClientCtx *c){
    pthread_mutex_lock(&c->mtx);
    c->joined = 0;
    c->current_group[0] = '\0';
    c->redirect_pending = 0;
    c->group_deleted = 0;
    pthread_mutex_unlock(&c->mtx);

    ui_send(c, "UI BANNER_ADMIN_CLR");
    ui_send(c, "UI BANNER_IDLE_CLR");
    ui_send(c, "UI CLRLOG");
    ui_set_header(c);
}

/* ───────────────────────── Server helpers ───────────────────────── */

/*
  Retour:
    1  => groupe trouvé (out_port rempli)
    0  => LIST reçu mais groupe absent
   -1  => pas de réponse / erreur réseau (NE PAS reset l’état sur ce cas)
*/
static int server_list_and_find(ClientCtx *c, const char *gname, uint16_t *out_port){
    const char *cmd = "LIST";

    // retry UDP (pertes possibles)
    for(int attempt=0; attempt<3; attempt++){
        if(sendto(c->sock_srv, cmd, strlen(cmd), 0, (struct sockaddr*)&c->srv_addr, sizeof c->srv_addr) < 0){
            return -1;
        }

        char buf[2048];
        struct sockaddr_in from; socklen_t fl = sizeof from;
        ssize_t n = recvfrom(c->sock_srv, buf, sizeof buf - 1, 0, (struct sockaddr*)&from, &fl);
        if(n <= 0){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                // retry
                continue;
            }
            if(errno == EINTR){
                // retry
                continue;
            }
            return -1;
        }

        buf[n] = '\0';

        char *save = NULL;
        char *line = strtok_r(buf, "\n", &save);
        while(line){
            char name[32]; unsigned port;
            if(sscanf(line, "%31s %u", name, &port) == 2){
                if(!strcmp(name, gname)){
                    if(out_port) *out_port = (uint16_t)port;
                    return 1;
                }
            }
            line = strtok_r(NULL, "\n", &save);
        }

        // LIST reçu mais pas trouvé
        return 0;
    }

    // aucun LIST reçu après retries
    return -1;
}

/* ───────────────────────── RX thread ───────────────────────── */

static void *rx_thread(void *arg){
    ClientCtx *c = (ClientCtx*)arg;
    char buf[TXT_LEN + 256];

    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 300000;
    setsockopt(c->sock_rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    while(!c->stop_rx){
        struct sockaddr_in from; socklen_t fl = sizeof from;
        ssize_t n = recvfrom(c->sock_rx, buf, sizeof buf - 1, 0, (struct sockaddr*)&from, &fl);
        if(n < 0){
            if(errno == EINTR) continue;
            if(errno == EAGAIN || errno == EWOULDBLOCK) continue;
            continue;
        }
        buf[n] = '\0';

        // CTRL: toujours traité
        if(!strncmp(buf, "CTRL ", 5)){
            if(!strncmp(buf, "CTRL BANNER_SET ", 16)){
                ui_send(c, "UI BANNER_ADMIN_SET %s", buf + 16);
                continue;
            }
            if(!strcmp(buf, "CTRL BANNER_CLR")){
                ui_send(c, "UI BANNER_ADMIN_CLR");
                continue;
            }
            if(!strncmp(buf, "CTRL IBANNER_SET ", 18)){
                ui_send(c, "UI BANNER_IDLE_SET %s", buf + 18);
                continue;
            }
            if(!strcmp(buf, "CTRL IBANNER_CLR")){
                ui_send(c, "UI BANNER_IDLE_CLR");
                continue;
            }
            if(!strncmp(buf, "CTRL REDIRECT ", 14)){
                char ng[32]={0}, reason[128]={0};
                unsigned p = 0;
                const char *payload = buf + 14;

                char *tmp = strdup(payload);
                if(tmp){
                    char *save=NULL;
                    char *t1=strtok_r(tmp, " ", &save);
                    char *t2=strtok_r(NULL," ", &save);
                    char *t3=save;
                    if(t1) isy_strcpy(ng, sizeof ng, t1);
                    if(t2) p=(unsigned)atoi(t2);
                    if(t3 && *t3) isy_strcpy(reason, sizeof reason, t3);
                    free(tmp);
                }

                pthread_mutex_lock(&c->mtx);
                c->redirect_pending = 1;
                isy_strcpy(c->redirect_group, sizeof c->redirect_group, ng);
                c->redirect_port = (uint16_t)p;
                isy_strcpy(c->redirect_reason, sizeof c->redirect_reason, reason[0]?reason:"redirect");
                pthread_mutex_unlock(&c->mtx);

                if(c->in_dialogue){
                    ui_log(c, "SYS: redirect demande par le serveur… bascule automatique.");
                }
                continue;
            }

            if(c->in_dialogue){
                ui_log(c, "%s", buf);
            }
            continue;
        }

        if(!strncmp(buf, "SYS Le groupe est supprime", 26) || strstr(buf, "Le groupe est supprime")){
            pthread_mutex_lock(&c->mtx);
            c->group_deleted = 1;
            pthread_mutex_unlock(&c->mtx);

            if(c->in_dialogue){
                ui_log(c, "%s", buf);
            }
            continue;
        }

        if(c->in_dialogue){
            ui_log(c, "%s", buf);
        }
    }
    return NULL;
}

/* ───────────────────────── UI process spawn ───────────────────────── */

static int start_ui(ClientCtx *c){
    snprintf(c->fifo_in,  sizeof c->fifo_in,  "/tmp/isy_ui_in_%d",  (int)getpid());
    snprintf(c->fifo_out, sizeof c->fifo_out, "/tmp/isy_ui_out_%d", (int)getpid());

    unlink(c->fifo_in);
    unlink(c->fifo_out);

    if(mkfifo(c->fifo_in, 0600) < 0) return -1;
    if(mkfifo(c->fifo_out, 0600) < 0) return -1;

    pid_t p = fork();
    if(p < 0) return -1;

    if(p == 0){
        execl("./AffichageISY", "AffichageISY", c->fifo_in, c->fifo_out, (char*)NULL);
        _exit(127);
    }

    c->ui_pid = p;

    c->ui_in_fd = open(c->fifo_in, O_RDWR);
    if(c->ui_in_fd < 0) return -1;

    c->ui_out_fd = open(c->fifo_out, O_RDWR);
    if(c->ui_out_fd < 0) return -1;

    ui_set_header(c);
    ui_send(c, "UI CLRLOG");
    ui_help(c);
    return 0;
}

static void stop_ui(ClientCtx *c){
    if(c->ui_in_fd >= 0){
        ui_send(c, "UI QUIT");
        close(c->ui_in_fd);
        c->ui_in_fd = -1;
    }
    if(c->ui_out_fd >= 0){
        close(c->ui_out_fd);
        c->ui_out_fd = -1;
    }
    if(c->fifo_in[0]) unlink(c->fifo_in);
    if(c->fifo_out[0]) unlink(c->fifo_out);
}

/* ───────────────────────── Dialogue loop ───────────────────────── */

static void dialog_loop(ClientCtx *c){
    char line[512];
    int cmd_mode = 0;

    c->in_dialogue = 1;
    ui_log(c, "Tapez quit pour revenir au menu, cmd pour entrer une commande, msg pour revenir aux messages.");

    while(1){
        pthread_mutex_lock(&c->mtx);
        int joined = c->joined;
        int deleted = c->group_deleted;
        int redir = c->redirect_pending;
        pthread_mutex_unlock(&c->mtx);

        if(!joined) break;

        if(deleted){
            ui_log(c, "SYS: le groupe a ete supprime. Tapez quit pour revenir au menu.");
        }

        if(redir){
            char ng[32]; uint16_t np; char rs[128];
            pthread_mutex_lock(&c->mtx);
            isy_strcpy(ng, sizeof ng, c->redirect_group);
            np = c->redirect_port;
            isy_strcpy(rs, sizeof rs, c->redirect_reason);
            c->redirect_pending = 0;
            pthread_mutex_unlock(&c->mtx);

            group_send_left(c);
            ui_log(c, "SYS: redirect vers %s:%u (%s)", ng, (unsigned)np, rs);

            c->grp_addr.sin_family = AF_INET;
            c->grp_addr.sin_port   = htons(np);
            c->grp_addr.sin_addr   = c->srv_addr.sin_addr;

            pthread_mutex_lock(&c->mtx);
            isy_strcpy(c->current_group, sizeof c->current_group, ng);
            c->joined = 1;
            pthread_mutex_unlock(&c->mtx);

            ui_set_header(c);
            group_send_join_hello(c);
            continue;
        }

        if(!ui_readline(c, line, sizeof line)) break;
        trimnl(line);
        if(line[0] == '\0') continue;

        if(!strcmp(line, "quit")){
            break;
        }

        if(!strcmp(line, "cmd")){
            cmd_mode = 1;
            ui_log(c, "SYS: mode cmd actif. Tape 'help' pour les commandes, ou 'msg' pour revenir.");
            continue;
        }
        if(!strcmp(line, "msg")){
            cmd_mode = 0;
            ui_log(c, "SYS: retour au mode messages.");
            continue;
        }

        if(cmd_mode){
            if(!strcmp(line, "help")){ ui_help(c); continue; }
            if(!strcmp(line, "admin")){ token_print(c); continue; }
            if(!strncmp(line, "settoken ", 9)){
                char g[32]={0}, tok[ADMIN_TOKEN_LEN]={0};
                if(sscanf(line+9, "%31s %63s", g, tok) == 2){
                    token_set(c, g, tok);
                    ui_log(c, "SYS: token enregistre pour %s.", g);
                }else{
                    ui_log(c, "SYS: syntaxe: settoken <groupe> <token>");
                }
                continue;
            }
            if(!strncmp(line, "ban ", 4)){
                const char *victim = line + 4;
                const char *tok = token_get(c, c->current_group);
                if(!tok){ ui_log(c, "SYS: pas admin (token manquant)."); continue; }
                char out2[256];
                snprintf(out2, sizeof out2, "CMD BAN2 %s %s %s", tok, c->user, victim);
                sendto(c->sock_rx, out2, strlen(out2), 0, (struct sockaddr*)&c->grp_addr, sizeof c->grp_addr);
                ui_log(c, "SYS: commande BAN envoyee.");
                continue;
            }
            if(!strncmp(line, "unban ", 6)){
                const char *victim = line + 6;
                const char *tok = token_get(c, c->current_group);
                if(!tok){ ui_log(c, "SYS: pas admin (token manquant)."); continue; }
                char out2[256];
                snprintf(out2, sizeof out2, "CMD UNBAN2 %s %s %s", tok, c->user, victim);
                sendto(c->sock_rx, out2, strlen(out2), 0, (struct sockaddr*)&c->grp_addr, sizeof c->grp_addr);
                ui_log(c, "SYS: commande UNBAN envoyee.");
                continue;
            }
            if(!strncmp(line, "merge ", 6)){
                char A[32]={0}, B[32]={0};
                if(sscanf(line+6, "%31s %31s", A, B) != 2){
                    ui_log(c, "SYS: syntaxe: merge <A> <B>");
                    continue;
                }
                const char *tA = token_get(c, A);
                const char *tB = token_get(c, B);
                if(!tA || !tB){
                    ui_log(c, "SYS: tokens manquants (il faut admin sur A et B).");
                    continue;
                }
                char req[256];
                snprintf(req, sizeof req, "MERGE %s %s %s %s %s", c->user, tA, A, tB, B);
                sendto(c->sock_srv, req, strlen(req), 0, (struct sockaddr*)&c->srv_addr, sizeof c->srv_addr);

                char resp[256];
                struct sockaddr_in from; socklen_t fl = sizeof from;
                ssize_t n = recvfrom(c->sock_srv, resp, sizeof resp - 1, 0, (struct sockaddr*)&from, &fl);
                if(n > 0){
                    resp[n] = '\0';
                    ui_log(c, "%s", resp);
                }else{
                    ui_log(c, "SYS: merge envoye (pas de reponse immediate).");
                }
                continue;
            }

            ui_log(c, "SYS: commande inconnue. Tape 'help'.");
            continue;
        }

        char out[512];
        snprintf(out, sizeof out, "MSG %s %s", c->user, line);
        sendto(c->sock_rx, out, strlen(out), 0, (struct sockaddr*)&c->grp_addr, sizeof c->grp_addr);
    }

    c->in_dialogue = 0;
}

/* ───────────────────────── Signals ───────────────────────── */

static ClientCtx *g_ctx = NULL;

static void on_sig(int s){
    (void)s;
    g_running = 0;
    if(g_ctx){
        pthread_mutex_lock(&g_ctx->mtx);
        int joined = g_ctx->joined;
        pthread_mutex_unlock(&g_ctx->mtx);
        if(joined) group_send_left(g_ctx);
        ui_send(g_ctx, "UI QUIT");
    }
}

/* ───────────────────────── Main ───────────────────────── */

int main(int argc, char **argv){
    if(argc < 2){
        fprintf(stderr, "Usage: %s conf/client.conf\n", argv[0]);
        return 1;
    }

    typedef struct {
        char user[EME_LEN];
        char srv_ip[64];
        uint16_t srv_port;
        uint16_t local_port;
    } ClientConf;

    ClientConf conf;
    memset(&conf, 0, sizeof conf);
    isy_strcpy(conf.user, sizeof conf.user, "user");
    isy_strcpy(conf.srv_ip, sizeof conf.srv_ip, "127.0.0.1");
    conf.srv_port = 8000;
    conf.local_port = 9001;

    FILE *f = fopen(argv[1], "r");
    if(!f) die_perror("client conf");

    char line[256], k[64], v[128];
    while(fgets(line, sizeof line, f)){
        if(line[0]=='#' || strlen(line)<3) continue;
        if(sscanf(line, "%63[^=]=%127s", k, v) == 2){
            if(!strcmp(k,"USER")) isy_strcpy(conf.user, sizeof conf.user, v);
            else if(!strcmp(k,"SERVER_IP")) isy_strcpy(conf.srv_ip, sizeof conf.srv_ip, v);
            else if(!strcmp(k,"SERVER_PORT")) conf.srv_port = (uint16_t)atoi(v);
            else if(!strcmp(k,"LOCAL_RECV_PORT")) conf.local_port = (uint16_t)atoi(v);
        }
    }
    fclose(f);

    ClientCtx c;
    memset(&c, 0, sizeof c);
    pthread_mutex_init(&c.mtx, NULL);

    c.ui_in_fd = -1;
    c.ui_out_fd = -1;
    c.in_dialogue = 0;

    isy_strcpy(c.user, sizeof c.user, conf.user);

    g_ctx = &c;
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    c.sock_srv = socket(AF_INET, SOCK_DGRAM, 0);
    if(c.sock_srv < 0) die_perror("socket srv");

    // timeout plus confortable (évite faux négatifs)
    struct timeval tvs; tvs.tv_sec = 1; tvs.tv_usec = 0;
    setsockopt(c.sock_srv, SOL_SOCKET, SO_RCVTIMEO, &tvs, sizeof tvs);

    memset(&c.srv_addr, 0, sizeof c.srv_addr);
    c.srv_addr.sin_family = AF_INET;
    c.srv_addr.sin_port   = htons(conf.srv_port);
    if(inet_pton(AF_INET, conf.srv_ip, &c.srv_addr.sin_addr) != 1) die_perror("inet_pton SERVER_IP");

    c.sock_rx = socket(AF_INET, SOCK_DGRAM, 0);
    if(c.sock_rx < 0) die_perror("socket rx");

    struct sockaddr_in laddr; memset(&laddr, 0, sizeof laddr);
    laddr.sin_family = AF_INET;
    laddr.sin_port   = htons(conf.local_port);
    laddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(c.sock_rx, (struct sockaddr*)&laddr, sizeof laddr) < 0) die_perror("bind rx");

    if(start_ui(&c) < 0){
        perror("start_ui");
        return 1;
    }
    ui_set_header(&c);

    c.stop_rx = 0;
    if(pthread_create(&c.rx_th, NULL, rx_thread, &c) != 0) die_perror("pthread_create rx");

    char in[512];

    while(g_running){
        ui_menu(&c);
        if(!ui_readline(&c, in, sizeof in)) break;
        trimnl(in);

        if(!strcmp(in, "2")){
            const char *cmd = "LIST";
            sendto(c.sock_srv, cmd, strlen(cmd), 0, (struct sockaddr*)&c.srv_addr, sizeof c.srv_addr);

            char resp[4096];
            struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t n = recvfrom(c.sock_srv, resp, sizeof resp - 1, 0, (struct sockaddr*)&from, &fl);
            if(n > 0){
                resp[n]='\0';
                char *save=NULL;
                char *ln = strtok_r(resp, "\n", &save);
                while(ln){
                    ui_log(&c, "%s", ln);
                    ln = strtok_r(NULL, "\n", &save);
                }
            } else {
                ui_log(&c, "(pas de reponse LIST)");
            }
            continue;
        }

        if(!strcmp(in, "0")){
            ui_log(&c, "Saisire le nom du groupe :");
            if(!ui_readline(&c, in, sizeof in)) break;
            trimnl(in);
            if(!in[0]) continue;

            char req[128];
            snprintf(req, sizeof req, "CREATE %s %s", in, c.user);
            sendto(c.sock_srv, req, strlen(req), 0, (struct sockaddr*)&c.srv_addr, sizeof c.srv_addr);

            char resp[256];
            struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t n = recvfrom(c.sock_srv, resp, sizeof resp - 1, 0, (struct sockaddr*)&from, &fl);
            if(n <= 0){
                ui_log(&c, "ERR: pas de reponse");
                continue;
            }
            resp[n]='\0';

            char okg[32]={0}, tok[ADMIN_TOKEN_LEN]={0};
            unsigned p=0;
            int nb = sscanf(resp, "OK %31s %u %63s", okg, &p, tok);

            ui_log(&c, "%s", resp);

            if(nb == 3 && tok[0]){
                token_set(&c, okg, tok);
                ui_log(&c, "SYS: tu es ADMIN de %s. (cmd -> admin)", okg);
            } else {
                ui_log(&c, "SYS: aucun token recu -> pas admin.");
            }
            continue;
        }

        if(!strcmp(in, "1")){
            pthread_mutex_lock(&c.mtx);
            int already = c.joined;
            pthread_mutex_unlock(&c.mtx);

            if(already){
                ui_log(&c, "Vous etes deja dans un groupe. Utilisez 5 pour quitter le groupe.");
                continue;
            }

            ui_log(&c, "Saisire le nom du groupe :");
            if(!ui_readline(&c, in, sizeof in)) break;
            trimnl(in);
            if(!in[0]) continue;

            char req[256];
            snprintf(req, sizeof req, "JOIN %s %s 0.0.0.0 0", in, c.user);
            sendto(c.sock_srv, req, strlen(req), 0, (struct sockaddr*)&c.srv_addr, sizeof c.srv_addr);

            char resp[256];
            struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t n = recvfrom(c.sock_srv, resp, sizeof resp - 1, 0, (struct sockaddr*)&from, &fl);
            if(n <= 0){
                ui_log(&c, "Join failed (pas de reponse)");
                continue;
            }
            resp[n]='\0';

            unsigned p=0; char okg[32]={0};
            if(sscanf(resp, "OK %31s %u", okg, &p) != 2){
                ui_log(&c, "%s", resp);
                continue;
            }

            pthread_mutex_lock(&c.mtx);
            c.joined = 1;
            isy_strcpy(c.current_group, sizeof c.current_group, okg);
            c.grp_addr.sin_family = AF_INET;
            c.grp_addr.sin_port = htons((uint16_t)p);
            c.grp_addr.sin_addr = c.srv_addr.sin_addr;
            c.group_deleted = 0;
            pthread_mutex_unlock(&c.mtx);

            ui_send(&c, "UI CLRLOG");
            ui_send(&c, "UI BANNER_ADMIN_CLR");
            ui_send(&c, "UI BANNER_IDLE_CLR");
            ui_set_header(&c);

            ui_log(&c, "Connexion au groupe %s realisee.", okg);

            group_send_join_hello(&c);
            continue;
        }

        if(!strcmp(in, "3")){
            pthread_mutex_lock(&c.mtx);
            int joined = c.joined;
            char cg[32]; isy_strcpy(cg, sizeof cg, c.current_group);
            pthread_mutex_unlock(&c.mtx);

            if(!joined){
                ui_log(&c, "Rejoignez un groupe d'abord (option 1).");
                continue;
            }

            uint16_t dummy_port;
            int chk = server_list_and_find(&c, cg, &dummy_port);

            if(chk == 0){
                // LIST reçu, groupe absent => là oui on reset
                ui_log(&c, "Le groupe n'existe plus (supprime). Etat reset.");
                cleanup_joined_state(&c);
                continue;
            }
            if(chk < 0){
                // pas de réponse LIST => NE PAS reset, on laisse tenter le dialogue
                ui_log(&c, "SYS: serveur ne repond pas a LIST (UDP). On tente d'entrer en dialogue quand meme.");
            }

            ui_send(&c, "UI CLRLOG");
            dialog_loop(&c);

            pthread_mutex_lock(&c.mtx);
            int deleted = c.group_deleted;
            pthread_mutex_unlock(&c.mtx);

            if(deleted){
                cleanup_joined_state(&c);
            }
            continue;
        }

        if(!strcmp(in, "5")){
            pthread_mutex_lock(&c.mtx);
            int joined = c.joined;
            pthread_mutex_unlock(&c.mtx);

            if(!joined){
                ui_log(&c, "Vous n'etes dans aucun groupe.");
                continue;
            }
            group_send_left(&c);
            cleanup_joined_state(&c);
            ui_log(&c, "Groupe quitte.");
            continue;
        }

        if(!strcmp(in, "4")){
            break;
        }

        ui_log(&c, "Commande inconnue.");
    }

    pthread_mutex_lock(&c.mtx);
    int joined = c.joined;
    pthread_mutex_unlock(&c.mtx);
    if(joined) group_send_left(&c);

    c.stop_rx = 1;
    pthread_join(c.rx_th, NULL);

    close(c.sock_rx);
    close(c.sock_srv);

    stop_ui(&c);

    pthread_mutex_destroy(&c.mtx);
    return 0;
}
