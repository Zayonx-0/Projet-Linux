// src/ServeurISY.c
#include "Commun.h"
#include <sys/wait.h>
#include <signal.h>
#include <arpa/inet.h>
#include <unistd.h>

#define MAX_GROUPS_DEFAULT 32
#define NAME_LEN 32

typedef struct {
    int used;
    char name[NAME_LEN];
    uint16_t port;
    pid_t pid;
    struct sockaddr_in addr; // 127.0.0.1:port (canal admin vers GroupeISY local)
} GroupRec;

/* ───────── Config serveur ───────── */
typedef struct {
    char bind_ip[64];         // "0.0.0.0" pour Internet
    uint16_t server_port;     // port de contrôle (LIST/CREATE/JOIN)
    uint16_t base_port;       // premier port de groupe (UDP)
    unsigned max_groups;      // nombre max de groupes
    unsigned idle_timeout;    // IDLE_TIMEOUT_SEC injecté à GroupeISY
} ServerConf;

static int load_server_conf(const char *path, ServerConf *c){
    memset(c,0,sizeof *c);
    strncpy(c->bind_ip, "0.0.0.0", sizeof c->bind_ip - 1);
    c->server_port = 8000;
    c->base_port   = 8010;
    c->max_groups  = MAX_GROUPS_DEFAULT;
    c->idle_timeout = 1800;

    FILE *f=fopen(path,"r"); if(!f) return -1;
    char line[256], k[64], v[128];
    while(fgets(line,sizeof line,f)){
        if(line[0]=='#' || strlen(line)<3) continue;
        if(sscanf(line, "%63[^=]=%127s", k, v)==2){
            if(!strcmp(k,"SERVER_IP"))      strncpy(c->bind_ip, v, sizeof c->bind_ip -1);
            else if(!strcmp(k,"SERVER_PORT")) c->server_port = (uint16_t)atoi(v);
            else if(!strcmp(k,"BASE_PORT"))   c->base_port   = (uint16_t)atoi(v);
            else if(!strcmp(k,"MAX_GROUPS"))  c->max_groups  = (unsigned)atoi(v);
            else if(!strcmp(k,"GROUP_IDLE_TIMEOUT_SEC")) c->idle_timeout = (unsigned)atoi(v);
        }
    }
    fclose(f);
    if(c->max_groups==0 || c->max_groups>256) c->max_groups = MAX_GROUPS_DEFAULT;
    return 0;
}

/* ───────── Etat global ───────── */
static volatile sig_atomic_t running = 1;
static GroupRec *groups = NULL;
static unsigned GMAX = 0;
static int sock_ctrl = -1;          // socket UDP de contrôle (clients ↔ serveur)
static ServerConf gconf;

static void on_sigint(int s){ (void)s; running = 0; }

static void on_sigchld(int s){
    (void)s;
    for(;;){
        int status;
        pid_t p = waitpid(-1, &status, WNOHANG);
        if(p<=0) break;
        for(unsigned i=0;i<GMAX;i++){
            if(groups[i].used && groups[i].pid == p){
                fprintf(stderr, "[Serveur] Groupe '%s' (port %u) termine.\n",
                        groups[i].name, (unsigned)groups[i].port);
                groups[i].used = 0;
                groups[i].pid = -1;
            }
        }
    }
}

/* ───────── Helpers groupes ───────── */
static int find_group_by_name(const char *name){
    for(unsigned i=0;i<GMAX;i++){
        if(groups[i].used && !strcmp(groups[i].name, name)) return (int)i;
    }
    return -1;
}
static int find_free_slot(void){
    for(unsigned i=0;i<GMAX;i++) if(!groups[i].used) return (int)i;
    return -1;
}

static int spawn_group(const char *name, uint16_t port, unsigned idle_sec, pid_t *outpid){
    pid_t p = fork();
    if(p<0) return -1;
    if(p==0){
        char pstr[16], tstr[16];
        snprintf(pstr,sizeof pstr,"%u",(unsigned)port);
        snprintf(tstr,sizeof tstr,"%u",(unsigned)idle_sec);
        execl("./GroupeISY","GroupeISY",name,pstr,tstr,(char*)NULL);
        _exit(127);
    }
    *outpid = p;
    return 0;
}

static void broadcast_to_groups(const char *payload){
    for(unsigned i=0;i<GMAX;i++){
        if(!groups[i].used) continue;
        sendto(sock_ctrl, payload, strlen(payload), 0,
               (struct sockaddr*)&groups[i].addr, sizeof groups[i].addr);
    }
}

/* ───────── Thread input admin ─────────
   Commandes console:
     /banner <texte>     -> CTRL BANNER_SET à tous les groupes
     /banner_clr         -> CTRL BANNER_CLR
     /sys <texte>        -> SYS <texte> à tous les groupes
     /list               -> affiche la table groupes
     /quit               -> arrêt du serveur (Ctrl-C pareil)
*/
static void *admin_input_thread(void *arg){
    (void)arg;
    char line[1024];
    while(running && fgets(line,sizeof line, stdin)){
        trimnl(line);
        if(!strncmp(line,"/banner ",8)){
            char out[1100]; snprintf(out,sizeof out,"CTRL BANNER_SET %s", line+8);
            broadcast_to_groups(out);
            fprintf(stderr,"[Serveur] Banner SET broadcast.\n");
        }else if(!strcmp(line,"/banner_clr")){
            broadcast_to_groups("CTRL BANNER_CLR");
            fprintf(stderr,"[Serveur] Banner CLR broadcast.\n");
        }else if(!strncmp(line,"/sys ",5)){
            char out[1100]; snprintf(out,sizeof out,"SYS %s", line+5);
            broadcast_to_groups(out);
            fprintf(stderr,"[Serveur] SYS broadcast.\n");
        }else if(!strcmp(line,"/list")){
            fprintf(stderr,"[Serveur] Groupes actifs:\n");
            for(unsigned i=0;i<GMAX;i++){
                if(groups[i].used){
                    fprintf(stderr,"  - %s  %u  (pid=%d)\n",
                            groups[i].name, (unsigned)groups[i].port, (int)groups[i].pid);
                }
            }
        }else if(!strcmp(line,"/quit")){
            running = 0;
            break;
        }else if(line[0]){
            fprintf(stderr,"[Serveur] Commandes: /banner <txt> | /banner_clr | /sys <txt> | /list | /quit\n");
        }
    }
    return NULL;
}

/* ───────── Main ───────── */
int main(int argc, char **argv){
    if(argc<2){
        fprintf(stderr,"Usage: %s conf/server.conf\n", argv[0]);
        return 1;
    }
    if(load_server_conf(argv[1], &gconf)<0) die_perror("server conf");

    /* handlers portables (signal()) */
    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGCHLD, on_sigchld);

    // state
    GMAX = gconf.max_groups;
    groups = (GroupRec*)calloc(GMAX, sizeof *groups);
    if(!groups){ perror("calloc groups"); return 1; }

    // socket contrôle (UDP)
    sock_ctrl = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock_ctrl<0) die_perror("socket");

    int yes=1; setsockopt(sock_ctrl, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in srv={0};
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(gconf.server_port);
    if(!strcmp(gconf.bind_ip,"0.0.0.0")) srv.sin_addr.s_addr = htonl(INADDR_ANY);
    else if(inet_pton(AF_INET, gconf.bind_ip, &srv.sin_addr)!=1) die_perror("inet_pton bind");
    if(bind(sock_ctrl, (struct sockaddr*)&srv, sizeof srv)<0) die_perror("bind server");

    // thread input admin
    pthread_t th_in; pthread_create(&th_in, NULL, admin_input_thread, NULL);

    fprintf(stderr,"[Serveur] écoute UDP %s:%u  | groupes %u..%u  | idle=%us\n",
            gconf.bind_ip, (unsigned)gconf.server_port,
            (unsigned)gconf.base_port, (unsigned)(gconf.base_port+GMAX-1),
            gconf.idle_timeout);

    // boucle contrôle
    char buf[1024];
    while(running){
        struct sockaddr_in cli; socklen_t cl=sizeof cli;
        ssize_t n = recvfrom(sock_ctrl, buf, sizeof buf -1, 0, (struct sockaddr*)&cli, &cl);
        if(n<0){
            if(errno==EINTR) continue;
            die_perror("recvfrom");
        }
        buf[n]='\0';

        // Commandes clients: CREATE <name> | LIST | JOIN <name> <user> <cip> <cport>
        if(!strncmp(buf,"LIST",4)){
            char out[4096]; out[0]='\0';
            for(unsigned i=0;i<GMAX;i++){
                if(groups[i].used){
                    char line[64];
                    snprintf(line,sizeof line,"%s %u\n", groups[i].name, (unsigned)groups[i].port);
                    strncat(out,line,sizeof out - strlen(out) - 1);
                }
            }
            if(out[0]=='\0') strcpy(out,"(aucun)\n");
            sendto(sock_ctrl,out,strlen(out),0,(struct sockaddr*)&cli,cl);

        }else if(!strncmp(buf,"CREATE ",7)){
            char gname[NAME_LEN]={0};
            if(sscanf(buf+7,"%31s", gname)!=1) continue;

            int idx = find_group_by_name(gname);
            if(idx>=0){
                char out[128];
                snprintf(out,sizeof out,"OK %s %u", groups[idx].name, (unsigned)groups[idx].port);
                sendto(sock_ctrl,out,strlen(out),0,(struct sockaddr*)&cli,cl);
                continue;
            }
            int freei = find_free_slot();
            if(freei<0){
                const char *err = "ERR no_slot";
                sendto(sock_ctrl,err,strlen(err),0,(struct sockaddr*)&cli,cl);
                continue;
            }
            uint16_t port = gconf.base_port + (uint16_t)freei;
            pid_t pid;
            if(spawn_group(gname, port, gconf.idle_timeout, &pid)<0){
                const char *err = "ERR spawn";
                sendto(sock_ctrl,err,strlen(err),0,(struct sockaddr*)&cli,cl);
                continue;
            }
            groups[freei].used = 1;
            groups[freei].pid  = pid;
            groups[freei].port = port;
            strncpy(groups[freei].name, gname, NAME_LEN-1);

            // addr d'admin -> 127.0.0.1:port (le groupe écoute localement sur le serveur)
            memset(&groups[freei].addr,0,sizeof groups[freei].addr);
            groups[freei].addr.sin_family = AF_INET;
            groups[freei].addr.sin_port   = htons(port);
            inet_pton(AF_INET, "127.0.0.1", &groups[freei].addr.sin_addr);

            char out[128];
            snprintf(out,sizeof out,"OK %s %u", gname, (unsigned)port);
            sendto(sock_ctrl,out,strlen(out),0,(struct sockaddr*)&cli,cl);

        }else if(!strncmp(buf,"JOIN ",5)){
            char gname[NAME_LEN]={0}, user[EME_LEN]={0}, cip[64]={0};
            unsigned cport=0;
            // On ignore cip/cport (le GroupeISY prend l'adresse depuis recvfrom)
            if(sscanf(buf+5,"%31s %19s %63s %u", gname, user, cip, &cport) < 2) continue;

            int idx = find_group_by_name(gname);
            if(idx<0){
                const char *err="ERR notfound";
                sendto(sock_ctrl,err,strlen(err),0,(struct sockaddr*)&cli,cl);
                continue;
            }
            char out[128];
            snprintf(out,sizeof out,"OK %s %u", groups[idx].name, (unsigned)groups[idx].port);
            sendto(sock_ctrl,out,strlen(out),0,(struct sockaddr*)&cli,cl);

        }else{
            // commande inconnue -> ignore
        }
    }

    // arrêt : tuer les groupes encore vivants
    fprintf(stderr,"[Serveur] arrêt…\n");
    for(unsigned i=0;i<GMAX;i++){
        if(groups[i].used){
            kill(groups[i].pid, SIGINT);
        }
    }
    // attendre la fin des enfants
    for(unsigned i=0;i<GMAX;i++){
        if(groups[i].used){
            int st; waitpid(groups[i].pid, &st, 0);
        }
    }

    if(sock_ctrl>=0) close(sock_ctrl);
    free(groups);
    return 0;
}
