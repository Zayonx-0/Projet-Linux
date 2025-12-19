// src/ClientISY.c
#include "Commun.h"
#include <sys/stat.h>
#include <limits.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <errno.h>

#define MAX_TOKENS 64
#define MAX_LOG_LINES 200
#define MAX_LINE 768

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

    // UI state
    char admin_banner[TXT_LEN];
    int  admin_banner_active;
    char idle_banner[TXT_LEN];
    int  idle_banner_active;

    // message log
    char log[MAX_LOG_LINES][MAX_LINE];
    int  log_count;

    // flags from rx thread
    volatile int ui_dirty;
    volatile int redirect_pending;
    char redirect_group[32];
    uint16_t redirect_port;
    char redirect_reason[128];

    volatile int group_deleted; // group says it's deleted due inactivity
    volatile int stop_rx;

    pthread_t rx_th;
    pthread_mutex_t mtx;
} ClientCtx;

/* ───────────────────────── Utils ───────────────────────── */

static void add_log(ClientCtx *c, const char *line){
    pthread_mutex_lock(&c->mtx);
    if(c->log_count < MAX_LOG_LINES){
        isy_strcpy(c->log[c->log_count], sizeof c->log[c->log_count], line);
        c->log_count++;
    } else {
        for(int i=1;i<MAX_LOG_LINES;i++){
            strcpy(c->log[i-1], c->log[i]);
        }
        isy_strcpy(c->log[MAX_LOG_LINES-1], sizeof c->log[MAX_LOG_LINES-1], line);
    }
    c->ui_dirty = 1;
    pthread_mutex_unlock(&c->mtx);
}

static int term_width(void){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0){
        if(ws.ws_col > 10) return (int)ws.ws_col;
    }
    return 80;
}

static void wrap_print(const char *s, int width){
    if(width < 20) width = 20;
    int col = 0;
    for(const char *p=s; *p; p++){
        putchar(*p);
        col++;
        if(*p=='\n'){ col=0; continue; }
        if(col >= width){
            putchar('\n');
            col=0;
        }
    }
    if(col) putchar('\n');
}

static void redraw(ClientCtx *c, int show_prompt){
    pthread_mutex_lock(&c->mtx);

    printf("\033[2J\033[H");

    int w = term_width();

    if(c->admin_banner_active){
        printf("=== BANNIERE ADMIN (SERVEUR) ===\n");
        wrap_print(c->admin_banner, w);
        printf("\n");
    }
    if(c->idle_banner_active){
        printf("=== BANNIERE INACTIVITE ===\n");
        wrap_print(c->idle_banner, w);
        printf("\n");
    }

    if(c->joined){
        printf("=== GROUPE: %s | USER: %s ===\n\n", c->current_group, c->user);
    }else{
        printf("=== PAS DANS UN GROUPE | USER: %s ===\n\n", c->user);
    }

    for(int i=0;i<c->log_count;i++){
        puts(c->log[i]);
    }

    if(show_prompt && c->joined){
        printf("\nMessage [%s] : ", c->current_group);
        fflush(stdout);
    }

    c->ui_dirty = 0;
    pthread_mutex_unlock(&c->mtx);
}

/* ───────────────────────── Help / Admin output (FIX UI) ─────────────────────────
   IMPORTANT: on n'imprime plus avec puts/printf, sinon redraw efface tout.
   On injecte dans le log.
*/

static void ui_help_to_log(ClientCtx *c){
    add_log(c, "=== AIDE CLIENTISY (admin / fusion / moderation) ===");
    add_log(c, "Devenir ADMIN (gestionnaire) :");
    add_log(c, "  - Cree le groupe via option 0 (Creation de groupe).");
    add_log(c, "  - Le client envoie: CREATE <groupe> <tonPseudo>");
    add_log(c, "  - Si le serveur renvoie un token, il est enregistre automatiquement.");
    add_log(c, "Commandes en mode 'cmd' (dans dialogue) :");
    add_log(c, "  help                         -> affiche cette aide");
    add_log(c, "  admin                        -> liste les tokens admin");
    add_log(c, "  settoken <groupe> <token>     -> importe un token manuellement");
    add_log(c, "  ban <pseudo>                  -> bannit un membre (kick + empeche de revenir)");
    add_log(c, "  unban <pseudo>                -> retire le ban");
    add_log(c, "  merge <A> <B>                 -> fusionne B vers A (admin des 2 requis)");
    add_log(c, "  msg                          -> retour a l'envoi de messages");
    add_log(c, "  quit                         -> retour au menu principal");
    add_log(c, "Notes:");
    add_log(c, "  - ban/unban: il faut etre admin du groupe courant (token present).");
    add_log(c, "  - merge A B: il faut etre admin de A et de B (deux tokens).");
    add_log(c, "====================================================");
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
    if(e){
        isy_strcpy(e->token, sizeof e->token, token);
    }
    c->ui_dirty = 1;
    pthread_mutex_unlock(&c->mtx);
}

static const char* token_get(ClientCtx *c, const char *group){
    TokenEntry *e = token_find(c, group);
    return e ? e->token : NULL;
}

static void token_print_to_log(ClientCtx *c){
    add_log(c, "=== TOKENS ADMIN ENREGISTRES ===");
    int any = 0;
    for(int i=0;i<MAX_TOKENS;i++){
        if(c->tokens[i].has){
            char tmp[256];
            snprintf(tmp, sizeof tmp, "  - %s : %s", c->tokens[i].group, c->tokens[i].token);
            add_log(c, tmp);
            any = 1;
        }
    }
    if(!any) add_log(c, "  (aucun token)");
    add_log(c, "===============================");
}

/* ───────────────────────── Server helpers ───────────────────────── */

static int server_list_and_find(ClientCtx *c, const char *gname, uint16_t *out_port){
    const char *cmd = "LIST";
    if(sendto(c->sock_srv, cmd, strlen(cmd), 0, (struct sockaddr*)&c->srv_addr, sizeof c->srv_addr) < 0)
        return 0;

    char buf[2048];
    struct sockaddr_in from; socklen_t fl = sizeof from;
    ssize_t n = recvfrom(c->sock_srv, buf, sizeof buf - 1, 0, (struct sockaddr*)&from, &fl);
    if(n <= 0) return 0;
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
    return 0;
}

/* ───────────────────────── Group join/leave ───────────────────────── */

static void group_send_join_hello(ClientCtx *c){
    char hello[128];
    snprintf(hello, sizeof hello, "MSG %s %s", c->user, "(joined)");
    (void)sendto(c->sock_rx, hello, strlen(hello), 0, (struct sockaddr*)&c->grp_addr, sizeof c->grp_addr);
}

static void group_send_left(ClientCtx *c){
    char bye[128];
    snprintf(bye, sizeof bye, "MSG %s %s", c->user, "(left)");
    (void)sendto(c->sock_rx, bye, strlen(bye), 0, (struct sockaddr*)&c->grp_addr, sizeof c->grp_addr);
}

static void cleanup_joined_state(ClientCtx *c){
    pthread_mutex_lock(&c->mtx);
    c->joined = 0;
    c->current_group[0] = '\0';
    c->admin_banner_active = 0;
    c->idle_banner_active = 0;
    c->admin_banner[0] = '\0';
    c->idle_banner[0] = '\0';
    c->redirect_pending = 0;
    c->group_deleted = 0;
    c->ui_dirty = 1;
    pthread_mutex_unlock(&c->mtx);
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

        if(!strncmp(buf, "CTRL ", 5)){
            if(!strncmp(buf, "CTRL BANNER_SET ", 16)){
                pthread_mutex_lock(&c->mtx);
                isy_strcpy(c->admin_banner, sizeof c->admin_banner, buf + 16);
                c->admin_banner_active = 1;
                c->ui_dirty = 1;
                pthread_mutex_unlock(&c->mtx);
                continue;
            }
            if(!strcmp(buf, "CTRL BANNER_CLR")){
                pthread_mutex_lock(&c->mtx);
                c->admin_banner_active = 0;
                c->admin_banner[0] = '\0';
                c->ui_dirty = 1;
                pthread_mutex_unlock(&c->mtx);
                continue;
            }
            if(!strncmp(buf, "CTRL IBANNER_SET ", 18)){
                pthread_mutex_lock(&c->mtx);
                isy_strcpy(c->idle_banner, sizeof c->idle_banner, buf + 18);
                c->idle_banner_active = 1;
                c->ui_dirty = 1;
                pthread_mutex_unlock(&c->mtx);
                continue;
            }
            if(!strcmp(buf, "CTRL IBANNER_CLR")){
                pthread_mutex_lock(&c->mtx);
                c->idle_banner_active = 0;
                c->idle_banner[0] = '\0';
                c->ui_dirty = 1;
                pthread_mutex_unlock(&c->mtx);
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
                c->ui_dirty = 1;
                pthread_mutex_unlock(&c->mtx);

                add_log(c, "SYS: fusion/redirect demande par le serveur… bascule automatique imminente.");
                continue;
            }

            add_log(c, buf);
            continue;
        }

        if(!strncmp(buf, "SYS Le groupe est supprime", 26) || strstr(buf, "Le groupe est supprime")){
            pthread_mutex_lock(&c->mtx);
            c->group_deleted = 1;
            c->ui_dirty = 1;
            pthread_mutex_unlock(&c->mtx);
        }

        add_log(c, buf);
    }
    return NULL;
}

/* ───────────────────────── Menu ───────────────────────── */

static void menu(void){
    puts("Choix des commandes :");
    puts("0 Creation de groupe");
    puts("1 Rejoindre un groupe");
    puts("2 Lister les groupes");
    puts("3 Dialoguer sur un groupe");
    puts("4 Quitter");
    puts("5 Quitter le groupe");
}

static void on_sig(int signo){
    (void)signo;
    _exit(0);
}

/* ───────────────────────── Dialogue loop ───────────────────────── */

static void dialog_loop(ClientCtx *c){
    char line[512];
    int cmd_mode = 0;

    pthread_mutex_lock(&c->mtx);
    c->ui_dirty = 1;
    pthread_mutex_unlock(&c->mtx);

    while(c->joined){
        if(c->ui_dirty){
            redraw(c, 1);
        }

        if(c->group_deleted){
            add_log(c, "SYS: le groupe a ete supprime. Tapez quit pour revenir au menu.");
        }

        if(c->redirect_pending){
            char ng[32]; uint16_t np; char rs[128];
            pthread_mutex_lock(&c->mtx);
            isy_strcpy(ng, sizeof ng, c->redirect_group);
            np = c->redirect_port;
            isy_strcpy(rs, sizeof rs, c->redirect_reason);
            c->redirect_pending = 0;
            pthread_mutex_unlock(&c->mtx);

            group_send_left(c);

            char info[256];
            snprintf(info, sizeof info, "SYS: redirect vers %s:%u (%s)", ng, (unsigned)np, rs);
            add_log(c, info);

            c->grp_addr.sin_family = AF_INET;
            c->grp_addr.sin_port   = htons(np);
            c->grp_addr.sin_addr = c->srv_addr.sin_addr;

            pthread_mutex_lock(&c->mtx);
            isy_strcpy(c->current_group, sizeof c->current_group, ng);
            c->joined = 1;
            c->ui_dirty = 1;
            pthread_mutex_unlock(&c->mtx);

            group_send_join_hello(c);
            continue;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000;

        int r = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        if(r < 0){
            if(errno == EINTR) continue;
            break;
        }
        if(r == 0){
            continue;
        }

        if(!FD_ISSET(STDIN_FILENO, &rfds)) continue;
        if(!fgets(line, sizeof line, stdin)) break;
        trimnl(line);

        if(line[0] == '\0'){
            continue;
        }

        if(!strcmp(line, "quit")){
            break;
        }

        if(!strcmp(line, "cmd")){
            cmd_mode = 1;
            add_log(c, "SYS: mode cmd actif. Tape 'help' pour les commandes, ou 'msg' pour revenir.");
            continue;
        }
        if(!strcmp(line, "msg")){
            cmd_mode = 0;
            add_log(c, "SYS: retour au mode messages.");
            continue;
        }

        if(cmd_mode){
            if(!strcmp(line, "help")){
                ui_help_to_log(c);
                continue;
            }
            if(!strcmp(line, "admin")){
                token_print_to_log(c);
                continue;
            }

            // NEW: import token manually
            if(!strncmp(line, "settoken ", 9)){
                char g[32]={0}, tok[ADMIN_TOKEN_LEN]={0};
                if(sscanf(line+9, "%31s %63s", g, tok) == 2){
                    token_set(c, g, tok);
                    char msg[256];
                    snprintf(msg, sizeof msg, "SYS: token enregistre pour %s.", g);
                    add_log(c, msg);
                }else{
                    add_log(c, "SYS: syntaxe: settoken <groupe> <token>");
                }
                continue;
            }

            if(!strncmp(line, "ban ", 4)){
                const char *victim = line + 4;
                const char *tok = token_get(c, c->current_group);
                if(!tok){
                    add_log(c, "SYS: tu n'es pas admin de ce groupe (pas de token).");
                    continue;
                }
                char out[256];
                snprintf(out, sizeof out, "CMD BAN %s %s", tok, victim);
                sendto(c->sock_rx, out, strlen(out), 0, (struct sockaddr*)&c->grp_addr, sizeof c->grp_addr);
                add_log(c, "SYS: commande BAN envoyee.");
                continue;
            }

            if(!strncmp(line, "unban ", 6)){
                const char *victim = line + 6;
                const char *tok = token_get(c, c->current_group);
                if(!tok){
                    add_log(c, "SYS: tu n'es pas admin de ce groupe (pas de token).");
                    continue;
                }
                char out[256];
                snprintf(out, sizeof out, "CMD UNBAN %s %s", tok, victim);
                sendto(c->sock_rx, out, strlen(out), 0, (struct sockaddr*)&c->grp_addr, sizeof c->grp_addr);
                add_log(c, "SYS: commande UNBAN envoyee.");
                continue;
            }

            if(!strncmp(line, "merge ", 6)){
                char A[32]={0}, B[32]={0};
                if(sscanf(line+6, "%31s %31s", A, B) != 2){
                    add_log(c, "SYS: syntaxe: merge <A> <B>");
                    continue;
                }
                const char *tA = token_get(c, A);
                const char *tB = token_get(c, B);
                if(!tA || !tB){
                    add_log(c, "SYS: il faut etre admin des 2 groupes (tokens manquants).");
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
                    add_log(c, resp);
                } else {
                    add_log(c, "SYS: merge envoye (pas de reponse immediate).");
                }
                continue;
            }

            add_log(c, "SYS: commande inconnue. Tape 'help'.");
            continue;
        }

        {
            char out[512];
            snprintf(out, sizeof out, "MSG %s %s", c->user, line);
            sendto(c->sock_rx, out, strlen(out), 0, (struct sockaddr*)&c->grp_addr, sizeof c->grp_addr);
        }
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

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    ClientCtx c;
    memset(&c, 0, sizeof c);
    pthread_mutex_init(&c.mtx, NULL);

    isy_strcpy(c.user, sizeof c.user, conf.user);

    c.sock_srv = socket(AF_INET, SOCK_DGRAM, 0);
    if(c.sock_srv < 0) die_perror("socket srv");

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

    c.stop_rx = 0;
    if(pthread_create(&c.rx_th, NULL, rx_thread, &c) != 0) die_perror("pthread_create rx");

    // Affiche l'aide une fois au démarrage dans le log (pas en stdout)
    ui_help_to_log(&c);

    for(;;){
        menu();
        printf("Choix : ");
        fflush(stdout);
        if(!fgets(line, sizeof line, stdin)) break;
        trimnl(line);

        if(!strcmp(line, "2")){
            const char *cmd = "LIST";
            sendto(c.sock_srv, cmd, strlen(cmd), 0, (struct sockaddr*)&c.srv_addr, sizeof c.srv_addr);

            char resp[4096];
            struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t n = recvfrom(c.sock_srv, resp, sizeof resp - 1, 0, (struct sockaddr*)&from, &fl);
            if(n > 0){
                resp[n]='\0';
                puts(resp);
            } else {
                puts("(pas de reponse)");
            }
            continue;
        }

        if(!strcmp(line, "0")){
            char gname[32];
            printf("Saisire le nom du groupe\n");
            if(!fgets(gname, sizeof gname, stdin)) continue;
            trimnl(gname);

            char req[128];
            snprintf(req, sizeof req, "CREATE %s %s", gname, c.user);
            sendto(c.sock_srv, req, strlen(req), 0, (struct sockaddr*)&c.srv_addr, sizeof c.srv_addr);

            char resp[256];
            struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t n = recvfrom(c.sock_srv, resp, sizeof resp - 1, 0, (struct sockaddr*)&from, &fl);
            if(n <= 0){ puts("ERR: pas de reponse"); continue; }
            resp[n]='\0';

            char okg[32]={0}, tok[ADMIN_TOKEN_LEN]={0};
            unsigned p=0;
            int nb = sscanf(resp, "OK %31s %u %63s", okg, &p, tok);
            puts(resp);

            if(nb == 3 && tok[0]){
                token_set(&c, okg, tok);
                char msg[256];
                snprintf(msg, sizeof msg, "SYS: tu es ADMIN de %s. Token enregistre. (cmd -> admin)", okg);
                add_log(&c, msg);
            } else {
                add_log(&c, "SYS: aucun token recu -> pas admin.");
            }
            continue;
        }

        if(!strcmp(line, "1")){
            if(c.joined){
                puts("Vous etes deja dans un groupe. Utilisez 5 pour quitter le groupe.");
                continue;
            }
            char gname[32];
            printf("Saisire le nom du groupe\n");
            if(!fgets(gname, sizeof gname, stdin)) continue;
            trimnl(gname);

            char req[256];
            snprintf(req, sizeof req, "JOIN %s %s 0.0.0.0 0", gname, c.user);
            sendto(c.sock_srv, req, strlen(req), 0, (struct sockaddr*)&c.srv_addr, sizeof c.srv_addr);

            char resp[256];
            struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t n = recvfrom(c.sock_srv, resp, sizeof resp - 1, 0, (struct sockaddr*)&from, &fl);
            if(n <= 0){ puts("Join failed"); continue; }
            resp[n]='\0';

            unsigned p=0; char okg[32]={0};
            if(sscanf(resp, "OK %31s %u", okg, &p) != 2){
                puts(resp);
                continue;
            }

            pthread_mutex_lock(&c.mtx);
            c.joined = 1;
            isy_strcpy(c.current_group, sizeof c.current_group, okg);
            c.grp_addr.sin_family = AF_INET;
            c.grp_addr.sin_port = htons((uint16_t)p);
            c.grp_addr.sin_addr = c.srv_addr.sin_addr;
            c.log_count = 0;
            c.admin_banner_active = 0; c.idle_banner_active = 0;
            c.admin_banner[0] = '\0'; c.idle_banner[0] = '\0';
            c.group_deleted = 0;
            c.ui_dirty = 1;
            pthread_mutex_unlock(&c.mtx);

            printf("Connexion au groupe %s realisee.\n", okg);

            group_send_join_hello(&c);
            continue;
        }

        if(!strcmp(line, "3")){
            if(!c.joined){
                puts("Rejoignez un groupe d'abord (option 1).");
                continue;
            }
            uint16_t dummy_port;
            if(!server_list_and_find(&c, c.current_group, &dummy_port)){
                puts("Le groupe n'existe plus (supprime). Etat reset.");
                cleanup_joined_state(&c);
                continue;
            }
            dialog_loop(&c);

            if(c.group_deleted){
                cleanup_joined_state(&c);
            }
            continue;
        }

        if(!strcmp(line, "5")){
            if(!c.joined){
                puts("Vous n'etes dans aucun groupe.");
                continue;
            }
            group_send_left(&c);
            cleanup_joined_state(&c);
            puts("Groupe quitte.");
            continue;
        }

        if(!strcmp(line, "4")){
            break;
        }

        puts("Commande inconnue.");
    }

    if(c.joined){
        group_send_left(&c);
    }
    c.stop_rx = 1;
    pthread_join(c.rx_th, NULL);
    close(c.sock_rx);
    close(c.sock_srv);
    pthread_mutex_destroy(&c.mtx);
    return 0;
}
