// src/ServeurISY.c
#include "Commun.h"

#define MAX_GROUPS 64

typedef struct {
    char name[32];
    char moderator[EME_LEN];
    uint16_t port;
    pid_t pid;         // PID du processus GroupeISY
    int active;
} SrvGroup;

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t need_reap = 0;

/* ───────── Signals ───────── */
static void on_sigint(int signo){ (void)signo; running = 0; }
static void on_sigchld(int signo){ (void)signo; need_reap = 1; }

/* ───────── Utils ───────── */
static void set_rcv_timeout(int sock, int ms){
    struct timeval tv; tv.tv_sec=ms/1000; tv.tv_usec=(ms%1000)*1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static int load_conf(const char *path,
                     char *srv_ip, uint16_t *srv_port,
                     uint16_t *base_port, int *max_groups,
                     unsigned *idle_timeout_sec)
{
    FILE *f = fopen(path, "r"); if(!f) return -1;
    char line[256], k[64], v[128];
    while (fgets(line, sizeof line, f)) {
        if(line[0]=='#' || strlen(line)<3) continue;
        if (sscanf(line, "%63[^=]=%127s", k, v)==2) {
            if (!strcmp(k, "SERVER_IP")) strncpy(srv_ip, v, 63);
            else if (!strcmp(k, "SERVER_PORT")) *srv_port = (uint16_t)atoi(v);
            else if (!strcmp(k, "GROUP_BASE_PORT")) *base_port = (uint16_t)atoi(v);
            else if (!strcmp(k, "MAX_GROUPS")) *max_groups = atoi(v);
            else if (!strcmp(k, "IDLE_TIMEOUT_SEC")) *idle_timeout_sec = (unsigned)atoi(v);
        }
    }
    fclose(f);
    return 0;
}

/* ───────── Broadcast helper (admin console -> groupes) ───────── */
static void send_to_all_groups(int sock, const char *srv_ip, SrvGroup *groups, int max_groups, const char *payload){
    for(int i=0;i<max_groups;i++){
        if(!groups[i].active) continue;
        struct sockaddr_in g = {0};
        g.sin_family = AF_INET;
        g.sin_port   = htons(groups[i].port);
        if(inet_pton(AF_INET, srv_ip, &g.sin_addr) != 1) continue;
        (void)sendto(sock, payload, strlen(payload), 0, (struct sockaddr*)&g, sizeof g);
    }
}

/* ───────── Admin console thread ─────────
   Commandes:
     /ban <texte>   → CTRL BANNER_SET <texte>
     /banoff        → CTRL BANNER_CLR
     <texte libre>  → SYS <texte> (non-collant)
*/
typedef struct {
    int sock;
    char srv_ip[64];
    SrvGroup *groups;
    int max_groups;
    volatile sig_atomic_t *runflag;
} AdminCtx;

static void *admin_thread(void *arg){
    AdminCtx *ctx = (AdminCtx*)arg;
    char *line = NULL; size_t cap = 0;

    fprintf(stdout,
        "[ADMIN] Commandes:\n"
        "  /ban <texte>   → bannière collante pour tous\n"
        "  /banoff        → retirer la bannière\n"
        "  <texte libre>  → message live non collant (SYS)\n");
    fflush(stdout);

    while(*(ctx->runflag)){
        errno = 0;
        ssize_t n = getline(&line, &cap, stdin);
        if(n <= 0){
            if(errno == EINTR) continue;
            break; // EOF
        }
        trimnl(line);
        if(line[0] == '\0') continue;

        if(!strncmp(line, "/banoff", 7)){
            char payload[] = "CTRL BANNER_CLR";
            send_to_all_groups(ctx->sock, ctx->srv_ip, ctx->groups, ctx->max_groups, payload);
            fprintf(stdout, "[ADMIN] Bannière retirée.\n"); fflush(stdout);
        }else if(!strncmp(line, "/ban ", 5)){
            char payload[512];
            snprintf(payload, sizeof payload, "CTRL BANNER_SET %s", line+5);
            send_to_all_groups(ctx->sock, ctx->srv_ip, ctx->groups, ctx->max_groups, payload);
            fprintf(stdout, "[ADMIN] Bannière posée.\n"); fflush(stdout);
        }else{
            char payload[512];
            snprintf(payload, sizeof payload, "SYS %s", line);
            send_to_all_groups(ctx->sock, ctx->srv_ip, ctx->groups, ctx->max_groups, payload);
        }
    }

    free(line);
    return NULL;
}

/* ───────── Reap children + nettoyer la table des groupes ───────── */
static void reap_dead_groups(SrvGroup *groups, int max_groups){
    int status;
    pid_t pid;
    while((pid = waitpid(-1, &status, WNOHANG)) > 0){
        for(int i=0;i<max_groups;i++){
            if(groups[i].active && groups[i].pid == pid){
                groups[i].active = 0;
                groups[i].pid = 0;
                groups[i].name[0] = '\0';
                groups[i].moderator[0] = '\0';
                groups[i].port = 0;
                fprintf(stdout, "ServeurISY: groupe fils %d terminé, slot libéré.\n", (int)pid);
                fflush(stdout);
                break;
            }
        }
    }
}

/* ───────── Main ───────── */
int main(int argc, char **argv){
    if(argc<2){ fprintf(stderr, "Usage: %s conf/server.conf\n", argv[0]); return 1; }

    /* Config par défaut */
    char srv_ip[64]="127.0.0.1";
    uint16_t srv_port=8000, base_port=8010;
    int max_groups=16;
    unsigned idle_timeout_sec = 1800; // 30 min par défaut

    if (load_conf(argv[1], srv_ip, &srv_port, &base_port, &max_groups, &idle_timeout_sec)<0)
        die_perror("server conf");
    if(max_groups > MAX_GROUPS) max_groups = MAX_GROUPS;

    /* Signals */
    struct sigaction sa; memset(&sa,0,sizeof sa);
    sa.sa_handler = on_sigint; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    struct sigaction sc; memset(&sc,0,sizeof sc);
    sc.sa_handler = on_sigchld; sigemptyset(&sc.sa_mask);
    sc.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sc, NULL);

    /* Socket UDP serveur */
    int s = socket(AF_INET, SOCK_DGRAM, 0); if(s<0) die_perror("socket");
    set_rcv_timeout(s, 500);
    struct sockaddr_in addr; memset(&addr,0,sizeof addr);
    addr.sin_family=AF_INET; addr.sin_port=htons(srv_port);
    if (inet_pton(AF_INET, srv_ip, &addr.sin_addr) != 1) die_perror("inet_pton");
    if (bind(s, (struct sockaddr*)&addr, sizeof addr)<0) die_perror("bind server");

    SrvGroup groups[MAX_GROUPS]; memset(groups,0,sizeof groups);
    uint16_t next_port = base_port;

    fprintf(stdout, "ServeurISY: %s:%u prêt (IDLE_TIMEOUT_SEC=%u)\n",
            srv_ip, (unsigned)srv_port, idle_timeout_sec);

    /* Thread admin console */
    AdminCtx actx = {.sock=s, .groups=groups, .max_groups=max_groups, .runflag=&running};
    strncpy(actx.srv_ip, srv_ip, sizeof actx.srv_ip - 1);
    pthread_t admin_th;
    if(pthread_create(&admin_th, NULL, admin_thread, &actx) != 0){
        perror("pthread_create(admin)");
    } else {
        pthread_detach(admin_th);
    }

    /* Boucle réseau */
    char buf[512];
    while (running) {
        if(need_reap){ reap_dead_groups(groups, max_groups); need_reap = 0; }

        struct sockaddr_in cli; socklen_t cl = sizeof cli;
        ssize_t n = recvfrom(s, buf, sizeof buf -1, 0, (struct sockaddr*)&cli, &cl);
        if(n<0){
            if(errno==EINTR) continue;
            if(errno==EAGAIN || errno==EWOULDBLOCK) continue;
            die_perror("recvfrom");
        }
        buf[n]='\0';

        if(!strncmp(buf, "LIST", 4)){
            char out[1024]; out[0]='\0';
            int empty=1;
            for(int i=0;i<max_groups;i++){
                if(groups[i].active){
                    empty=0;
                    char line[96];
                    snprintf(line,sizeof line,"%s %u\n", groups[i].name, groups[i].port);
                    strncat(out,line,sizeof out - strlen(out) -1);
                }
            }
            if(empty) strcpy(out,"Aucun groupe\n");
            sendto(s, out, strlen(out), 0, (struct sockaddr*)&cli, cl);

        } else if(!strncmp(buf, "CREATE ", 7)){
            char gname[32]={0};
            if(sscanf(buf+7, "%31s", gname)!=1){
                const char*e="ERR bad CREATE\n"; sendto(s,e,strlen(e),0,(struct sockaddr*)&cli,cl); continue;
            }
            int slot=-1; for(int i=0;i<max_groups;i++) if(!groups[i].active){ slot=i; break; }
            if(slot<0){ const char*e="ERR no slot\n"; sendto(s,e,strlen(e),0,(struct sockaddr*)&cli,cl); continue; }
            int dup=0; for(int i=0;i<max_groups;i++) if(groups[i].active && !strcmp(groups[i].name,gname)) dup=1;
            if(dup){ const char*e="ERR exists\n"; sendto(s,e,strlen(e),0,(struct sockaddr*)&cli,cl); continue; }

            uint16_t gport = next_port++;
            pid_t pid = fork();
            if(pid<0) die_perror("fork");
            if(pid==0){
                char pbuf[16]; snprintf(pbuf,sizeof pbuf, "%u", gport);
                char tbuf[16]; snprintf(tbuf,sizeof tbuf, "%u", idle_timeout_sec);
                execl("./GroupeISY", "GroupeISY", gname, pbuf, tbuf, (char*)NULL);
                die_perror("execl GroupeISY");
            }
            strncpy(groups[slot].name, gname, sizeof groups[slot].name-1);
            groups[slot].moderator[0]='\0';
            groups[slot].port = gport;
            groups[slot].pid  = pid;
            groups[slot].active=1;
            char ok[64]; snprintf(ok, sizeof ok, "OK %s %u\n", gname, gport);
            sendto(s, ok, strlen(ok), 0, (struct sockaddr*)&cli, cl);

        } else if(!strncmp(buf, "JOIN ", 5)){
            char gname[32], user[32], cip[64]; int cport=0;
            if(sscanf(buf+5, "%31s %31s %63s %d", gname, user, cip, &cport)!=4){
                const char*e="ERR bad JOIN\n"; sendto(s,e,strlen(e),0,(struct sockaddr*)&cli,cl); continue;
            }
            int found=-1; for(int i=0;i<max_groups;i++) if(groups[i].active && !strcmp(groups[i].name,gname)){ found=i; break; }
            if(found<0){ const char*e="ERR no group\n"; sendto(s,e,strlen(e),0,(struct sockaddr*)&cli,cl); continue; }
            char ok[128]; snprintf(ok,sizeof ok, "OK %s %u\n", groups[found].name, groups[found].port);
            sendto(s, ok, strlen(ok), 0, (struct sockaddr*)&cli, cl);

        } else if(!strncmp(buf, "QUIT ", 5)){
            const char*ok="OK bye\n"; sendto(s,ok,strlen(ok),0,(struct sockaddr*)&cli,cl);

        } else {
            const char*e="ERR unknown\n"; sendto(s,e,strlen(e),0,(struct sockaddr*)&cli,cl);
        }
    }

    /* Reap restants (arrêt) */
    reap_dead_groups(groups, max_groups);
    close(s);
    fprintf(stdout, "ServeurISY: arrêt propre.\n");
    return 0;
}
