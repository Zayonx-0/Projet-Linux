// src/ServeurISY.c
#include "Commun.h"
#include <sys/wait.h>
#include <signal.h>
#include <arpa/inet.h>
#include <unistd.h>

/*
    ─────────────────────────────────────────────────────────────────────────
    ServeurISY
    ─────────────────────────────────────────────────────────────────────────
    Rôle :
      - Fournit un point d’entrée unique (UDP) pour :
          * LIST   : lister les groupes existants
          * CREATE : créer un groupe (lance un processus GroupeISY)
          * JOIN   : récupérer le port d’un groupe existant
          * MERGE  : fusionner deux groupes (rediriger les clients de B vers A)
      - Peut diffuser une bannière "serveur" ou des messages SYS à tous les groupes.

    Architecture :
      - Un socket UDP "contrôle" (sock_ctrl) sur SERVER_IP:SERVER_PORT
      - Un tableau de groupes en mémoire (groups[])
      - Chaque groupe est un processus enfant (fork + execl ./GroupeISY)
      - Canal admin serveur -> groupe :
          * Chaque GroupeISY écoute sur son port UDP (port du groupe)
          * Le serveur lui envoie des commandes "CTRL ..." vers 127.0.0.1:<port>
            (car groupe et serveur tournent sur la même machine).

    Signaux :
      - SIGINT/SIGTERM : stoppe la boucle principale proprement
      - SIGCHLD : récupère la mort des enfants (GroupeISY) et nettoie l’état.
*/

#define MAX_GROUPS_DEFAULT 32   // taille max par défaut si non spécifié
#define NAME_LEN 32             // longueur max du nom de groupe côté serveur

/*
    Enregistrement d’un groupe côté serveur.
    - used : indique si l’entrée est occupée
    - name / port : identité du groupe
    - pid : PID du processus GroupeISY lancé
    - addr : adresse admin (127.0.0.1:port) pour envoyer des CTRL au groupe
    - admin_token : token de gestionnaire (admin) attribué à la création
*/
typedef struct {
    int used;
    char name[NAME_LEN];
    uint16_t port;
    pid_t pid;
    struct sockaddr_in addr;            // 127.0.0.1:port (canal admin vers GroupeISY local)
    char admin_token[ADMIN_TOKEN_LEN];  // token admin (gestionnaire) du groupe
} GroupRec;

/* ───────────────────────── Configuration serveur ───────────────────────── */
/*
    Champs paramétrables via server.conf :
      - SERVER_IP (ou 0.0.0.0 pour écouter sur toutes les interfaces)
      - SERVER_PORT (port de contrôle)
      - BASE_PORT (premier port attribué aux groupes)
      - MAX_GROUPS
      - IDLE_TIMEOUT_SEC (timeout d’inactivité injecté à GroupeISY)
*/
typedef struct {
    char bind_ip[64];         // "0.0.0.0" pour Internet
    uint16_t server_port;     // port de contrôle (LIST/CREATE/JOIN/MERGE)
    uint16_t base_port;       // premier port de groupe (UDP)
    unsigned max_groups;      // nombre max de groupes
    unsigned idle_timeout;    // IDLE_TIMEOUT_SEC injecté à GroupeISY
} ServerConf;

/*
    Charge la configuration depuis un fichier texte (format KEY=VALUE),
    avec support des commentaires (# ...).
    Retour :
      0  -> OK
     -1 -> fichier inaccessible
*/
static int load_server_conf(const char *path, ServerConf *c){
    memset(c,0,sizeof *c);

    // Valeurs par défaut (si non présentes dans le fichier)
    strncpy(c->bind_ip, "0.0.0.0", sizeof c->bind_ip - 1);
    c->server_port  = 8000;
    c->base_port    = 8010;
    c->max_groups   = MAX_GROUPS_DEFAULT;
    c->idle_timeout = 1800; // valeur par défaut si absent du .conf

    FILE *f=fopen(path,"r");
    if(!f) return -1;

    char line[256], k[64], v[128];
    while(fgets(line,sizeof line,f)){
        // Supprime les commentaires éventuels en fin de ligne
        char *hash = strchr(line, '#');
        if(hash) *hash = '\0';

        // Ignore les lignes vides / trop courtes
        if(line[0]=='\0' || strlen(line)<3) continue;

        // Lecture simple KEY=VALUE
        if(sscanf(line, "%63[^=]=%127s", k, v)==2){
            if(!strcmp(k,"SERVER_IP"))
                strncpy(c->bind_ip, v, sizeof c->bind_ip -1);
            else if(!strcmp(k,"SERVER_PORT"))
                c->server_port = (uint16_t)atoi(v);
            else if(!strcmp(k,"BASE_PORT"))
                c->base_port   = (uint16_t)atoi(v);
            else if(!strcmp(k,"MAX_GROUPS"))
                c->max_groups  = (unsigned)atoi(v);
            else if(!strcmp(k,"IDLE_TIMEOUT_SEC"))
                c->idle_timeout = (unsigned)atoi(v);
        }
    }
    fclose(f);

    // Sécurisation : on borne la valeur max_groups
    if(c->max_groups==0 || c->max_groups>256) c->max_groups = MAX_GROUPS_DEFAULT;
    return 0;
}

/* ───────────────────────── Etat global ───────────────────────── */
/*
    running :
      - Flag modifié par SIGINT/SIGTERM pour quitter proprement la boucle principale.
    groups :
      - Table des groupes (taille GMAX), allouée dynamiquement.
    sock_ctrl :
      - Socket UDP principal du serveur.
    th_in :
      - Thread dédié à l’input admin (/banner, /sys, /list, /quit).
*/
static volatile sig_atomic_t running = 1;
static GroupRec *groups = NULL;
static unsigned GMAX = 0;
static int sock_ctrl = -1;
static ServerConf gconf;
static pthread_t th_in;

/* ───────────────────────── Gestion des signaux ───────────────────────── */

/*
    Interruption clavier (Ctrl-C) ou arrêt demandé :
      - On se contente de passer running à 0.
      - La boucle recvfrom() se réveille grâce à un timeout (SO_RCVTIMEO).
*/
static void on_sigint(int s){
    (void)s;
    running = 0;
}

/*
    SIGCHLD : un enfant (GroupeISY) s’est terminé.
    On récupère tous les enfants morts via waitpid(WNOHANG) et on libère les slots correspondants.
*/
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

                // Libère le slot
                groups[i].used = 0;
                groups[i].pid = -1;
                groups[i].admin_token[0] = '\0';
            }
        }
    }
}

/* ───────────────────────── Helpers groupes ───────────────────────── */

/* Recherche l’index d’un groupe par nom. Retourne -1 si absent. */
static int find_group_by_name(const char *name){
    for(unsigned i=0;i<GMAX;i++){
        if(groups[i].used && !strcmp(groups[i].name, name)) return (int)i;
    }
    return -1;
}

/* Retourne un slot libre dans la table groups (ou -1 si plein). */
static int find_free_slot(void){
    for(unsigned i=0;i<GMAX;i++){
        if(!groups[i].used) return (int)i;
    }
    return -1;
}

/*
    Lance un processus GroupeISY.
    Arguments :
      - name : nom du groupe
      - port : port UDP du groupe
      - idle_sec : timeout d’inactivité transmis au groupe
      - outpid : PID du processus enfant
*/
static int spawn_group(const char *name, uint16_t port, unsigned idle_sec, pid_t *outpid){
    pid_t p = fork();
    if(p<0) return -1;

    if(p==0){
        // Processus enfant : exécute GroupeISY
        char pstr[16], tstr[16];
        snprintf(pstr,sizeof pstr,"%u",(unsigned)port);
        snprintf(tstr,sizeof tstr,"%u",(unsigned)idle_sec);

        execl("./GroupeISY","GroupeISY",name,pstr,tstr,(char*)NULL);

        // Si execl échoue, on sort immédiatement (127 = convention)
        _exit(127);
    }

    // Processus parent
    *outpid = p;
    return 0;
}

/*
    Envoie un message (payload) à tous les groupes actifs.
    Exemple : diffusion d’une bannière ou d’un SYS.
*/
static void broadcast_to_groups(const char *payload){
    for(unsigned i=0;i<GMAX;i++){
        if(!groups[i].used) continue;

        (void)sendto(sock_ctrl, payload, strlen(payload), 0,
                     (struct sockaddr*)&groups[i].addr, sizeof groups[i].addr);
    }
}

/* ───────────────────────── Token generator ───────────────────────── */
/*
    Génère un token "admin" côté serveur.
    - Utilise /dev/urandom si disponible (préféré)
    - Sinon, fallback sur time + pid
    Format choisi : hex, pratique à copier-coller.
*/
static void gen_token(char out[ADMIN_TOKEN_LEN]){
    unsigned char rnd[16];

    int fd = open("/dev/urandom", O_RDONLY);
    if(fd >= 0){
        ssize_t n = read(fd, rnd, sizeof rnd);
        close(fd);

        if(n == (ssize_t)sizeof rnd){
            static const char *hx="0123456789abcdef";
            for(int i=0;i<16;i++){
                out[i*2]   = hx[(rnd[i]>>4)&0xF];
                out[i*2+1] = hx[(rnd[i])&0xF];
            }
            out[32] = '\0';
            return;
        }
    }

    // Fallback minimaliste (moins aléatoire mais suffisant pour un projet)
    unsigned long a = (unsigned long)time(NULL);
    unsigned long b = (unsigned long)getpid();
    snprintf(out, ADMIN_TOKEN_LEN, "%08lx%08lx", a, b);
}

/* ───────────────────────── Thread input admin ───────────────────────── */
/*
    Thread console :
      - Lit stdin du serveur
      - Envoie des commandes "CTRL" à tous les groupes
      - Affiche /list pour debug
      - /quit pour stopper proprement
    Note : on active l’annulation pthread_cancel pour pouvoir tuer le thread au shutdown.
*/
static void *admin_input_thread(void *arg){
    (void)arg;

    // Permet l'annulation (pthread_cancel) pendant fgets()
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    // Affiche une aide au démarrage du serveur
    fprintf(stderr,
        "[Serveur] Commandes admin:\n"
        "  /banner <txt>     -> bannière serveur (tous les groupes)\n"
        "  /banner_clr       -> retire bannière serveur\n"
        "  /sys <txt>        -> message SYS (tous les groupes)\n"
        "  /list             -> liste groupes actifs\n"
        "  /quit             -> arrêter le serveur (Ctrl-C aussi)\n"
    );

    char line[1024];

    while(running && fgets(line,sizeof line, stdin)){
        trimnl(line);

        if(!strncmp(line,"/banner ",8)){
            char out[1100];
            snprintf(out,sizeof out,"CTRL BANNER_SET %s", line+8);
            broadcast_to_groups(out);
            fprintf(stderr,"[Serveur] Banner SET broadcast.\n");

        }else if(!strcmp(line,"/banner_clr")){
            broadcast_to_groups("CTRL BANNER_CLR");
            fprintf(stderr,"[Serveur] Banner CLR broadcast.\n");

        }else if(!strncmp(line,"/sys ",5)){
            char out[1100];
            snprintf(out,sizeof out,"SYS %s", line+5);
            broadcast_to_groups(out);
            fprintf(stderr,"[Serveur] SYS broadcast.\n");

        }else if(!strcmp(line,"/list")){
            fprintf(stderr,"[Serveur] Groupes actifs:\n");
            for(unsigned i=0;i<GMAX;i++){
                if(groups[i].used){
                    fprintf(stderr,"  - %s  %u  (pid=%d) token=%s\n",
                            groups[i].name,
                            (unsigned)groups[i].port,
                            (int)groups[i].pid,
                            groups[i].admin_token[0] ? groups[i].admin_token : "(none)");
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

/* ───────────────────────── Main ───────────────────────── */
int main(int argc, char **argv){
    if(argc<2){
        fprintf(stderr,"Usage: %s conf/server.conf\n", argv[0]);
        return 1;
    }

    // Lecture config serveur (serveur.conf)
    if(load_server_conf(argv[1], &gconf)<0) die_perror("server conf");

    // Handlers simples (portables) : stop / nettoyage enfants
    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGCHLD, on_sigchld);

    // Alloue la table de groupes
    GMAX = gconf.max_groups;
    groups = (GroupRec*)calloc(GMAX, sizeof *groups);
    if(!groups){
        perror("calloc groups");
        return 1;
    }

    // Socket UDP de contrôle (clients <-> serveur)
    sock_ctrl = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock_ctrl<0) die_perror("socket");

    // Permet de relancer rapidement le serveur
    int yes=1;
    (void)setsockopt(sock_ctrl, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    /*
        Fix important pour Ctrl-C :
        - recvfrom() est bloquant, donc sans timeout la boucle ne peut pas voir running=0.
        - On ajoute un SO_RCVTIMEO => toutes les 300ms, recvfrom() se réveille.
    */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 300000; // 300ms
    (void)setsockopt(sock_ctrl, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    // Bind sur SERVER_IP:SERVER_PORT
    struct sockaddr_in srv={0};
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(gconf.server_port);

    if(!strcmp(gconf.bind_ip,"0.0.0.0")){
        srv.sin_addr.s_addr = htonl(INADDR_ANY);
    }else{
        if(inet_pton(AF_INET, gconf.bind_ip, &srv.sin_addr)!=1) die_perror("inet_pton bind");
    }

    if(bind(sock_ctrl, (struct sockaddr*)&srv, sizeof srv)<0) die_perror("bind server");

    // Démarre le thread d'input admin
    pthread_create(&th_in, NULL, admin_input_thread, NULL);

    fprintf(stderr,"[Serveur] écoute UDP %s:%u  | groupes %u..%u  | idle=%us\n",
            gconf.bind_ip,
            (unsigned)gconf.server_port,
            (unsigned)gconf.base_port,
            (unsigned)(gconf.base_port+GMAX-1),
            gconf.idle_timeout);

    /*
        Boucle principale UDP :
        - reçoit une commande
        - répond immédiatement (protocole simple)
        - CREATE peut lancer un enfant
    */
    char buf[1024];
    while(running){
        struct sockaddr_in cli;
        socklen_t cl=sizeof cli;

        ssize_t n = recvfrom(sock_ctrl, buf, sizeof buf -1, 0,
                             (struct sockaddr*)&cli, &cl);

        if(n<0){
            if(errno==EINTR){
                // Interruption signal : si arrêt demandé, on sort
                if(!running) break;
                continue;
            }
            if(errno==EAGAIN || errno==EWOULDBLOCK){
                // Timeout : on boucle juste pour relire running
                continue;
            }
            die_perror("recvfrom");
        }

        buf[n]='\0';

        /* ───────── LIST ───────── */
        if(!strncmp(buf,"LIST",4)){
            char out[4096];
            out[0]='\0';

            for(unsigned i=0;i<GMAX;i++){
                if(groups[i].used){
                    char line[64];
                    snprintf(line,sizeof line,"%s %u\n",
                             groups[i].name, (unsigned)groups[i].port);
                    strncat(out,line,sizeof out - strlen(out) - 1);
                }
            }

            if(out[0]=='\0') strcpy(out,"(aucun)\n");

            sendto(sock_ctrl,out,strlen(out),0,(struct sockaddr*)&cli,cl);
            continue;
        }

        /* ───────── CREATE <name> [user] ───────── */
        if(!strncmp(buf,"CREATE ",7)){
            char gname[NAME_LEN]={0};
            char user[EME_LEN]={0};

            // CREATE <group> <user> (user optionnel)
            int nb = sscanf(buf+7,"%31s %19s", gname, user);
            if(nb < 1) continue;

            // Si le groupe existe déjà : renvoyer le port (et le token si existant)
            int idx = find_group_by_name(gname);
            if(idx>=0){
                char out[256];
                if(groups[idx].admin_token[0]){
                    snprintf(out,sizeof out,"OK %s %u %s",
                             groups[idx].name,
                             (unsigned)groups[idx].port,
                             groups[idx].admin_token);
                }else{
                    snprintf(out,sizeof out,"OK %s %u",
                             groups[idx].name,
                             (unsigned)groups[idx].port);
                }
                sendto(sock_ctrl,out,strlen(out),0,(struct sockaddr*)&cli,cl);
                continue;
            }

            // Cherche un slot libre
            int freei = find_free_slot();
            if(freei<0){
                const char *err = "ERR no_slot";
                sendto(sock_ctrl,err,strlen(err),0,(struct sockaddr*)&cli,cl);
                continue;
            }

            // Port attribué = base_port + index du slot
            uint16_t port = gconf.base_port + (uint16_t)freei;

            // Lance le processus GroupeISY
            pid_t pid;
            if(spawn_group(gname, port, gconf.idle_timeout, &pid)<0){
                const char *err = "ERR spawn";
                sendto(sock_ctrl,err,strlen(err),0,(struct sockaddr*)&cli,cl);
                continue;
            }

            // Remplit le slot groupe
            groups[freei].used = 1;
            groups[freei].pid  = pid;
            groups[freei].port = port;
            strncpy(groups[freei].name, gname, NAME_LEN-1);

            // Canal admin local vers le groupe : 127.0.0.1:port
            memset(&groups[freei].addr,0,sizeof groups[freei].addr);
            groups[freei].addr.sin_family = AF_INET;
            groups[freei].addr.sin_port   = htons(port);
            inet_pton(AF_INET, "127.0.0.1", &groups[freei].addr.sin_addr);

            // Token admin uniquement si le client fournit un "user" (création via client moderne)
            groups[freei].admin_token[0] = '\0';
            if(nb >= 2){
                gen_token(groups[freei].admin_token);

                char out[256];
                snprintf(out,sizeof out,"OK %s %u %s",
                         gname, (unsigned)port, groups[freei].admin_token);
                sendto(sock_ctrl,out,strlen(out),0,(struct sockaddr*)&cli,cl);
            }else{
                // compatibilité / création "legacy" sans admin
                char out[128];
                snprintf(out,sizeof out,"OK %s %u",
                         gname, (unsigned)port);
                sendto(sock_ctrl,out,strlen(out),0,(struct sockaddr*)&cli,cl);
            }

            continue;
        }

        /* ───────── JOIN <name> <user> <cip> <cport> ───────── */
        if(!strncmp(buf,"JOIN ",5)){
            char gname[NAME_LEN]={0}, user[EME_LEN]={0}, cip[64]={0};
            unsigned cport=0;

            // cip/cport peuvent être ignorés (UDP, on récupère l’adresse via recvfrom côté groupe)
            if(sscanf(buf+5,"%31s %19s %63s %u", gname, user, cip, &cport) < 2) continue;

            int idx = find_group_by_name(gname);
            if(idx<0){
                const char *err="ERR notfound";
                sendto(sock_ctrl,err,strlen(err),0,(struct sockaddr*)&cli,cl);
                continue;
            }

            // Renvoie le port du groupe
            char out[128];
            snprintf(out,sizeof out,"OK %s %u", groups[idx].name, (unsigned)groups[idx].port);
            sendto(sock_ctrl,out,strlen(out),0,(struct sockaddr*)&cli,cl);
            continue;
        }

        /* ───────── MERGE <user> <tokenA> <groupA> <tokenB> <groupB> ───────── */
        if(!strncmp(buf,"MERGE ",6)){
            char user[EME_LEN]={0};
            char tokA[ADMIN_TOKEN_LEN]={0}, tokB[ADMIN_TOKEN_LEN]={0};
            char gA[NAME_LEN]={0}, gB[NAME_LEN]={0};

            // On vérifie la syntaxe exacte
            if(sscanf(buf+6, "%19s %63s %31s %63s %31s", user, tokA, gA, tokB, gB) != 5){
                const char *err="ERR merge_syntax";
                sendto(sock_ctrl,err,strlen(err),0,(struct sockaddr*)&cli,cl);
                continue;
            }

            // Les deux groupes doivent exister
            int iA = find_group_by_name(gA);
            int iB = find_group_by_name(gB);
            if(iA<0 || iB<0){
                const char *err="ERR notfound";
                sendto(sock_ctrl,err,strlen(err),0,(struct sockaddr*)&cli,cl);
                continue;
            }

            // Les deux groupes doivent avoir un token défini
            if(!groups[iA].admin_token[0] || !groups[iB].admin_token[0]){
                const char *err="ERR no_token";
                sendto(sock_ctrl,err,strlen(err),0,(struct sockaddr*)&cli,cl);
                continue;
            }

            // Vérification des tokens
            if(strcmp(groups[iA].admin_token, tokA)!=0 || strcmp(groups[iB].admin_token, tokB)!=0){
                const char *err="ERR bad_token";
                sendto(sock_ctrl,err,strlen(err),0,(struct sockaddr*)&cli,cl);
                continue;
            }

            /*
                Fusion : on demande au groupe B de rediriger ses clients vers A.
                Note : ici, la "fusion" est logique (redirect), pas un transfert complet d’historique.
            */
            char ctrl[512];
            snprintf(ctrl, sizeof ctrl, "CTRL REDIRECT %s %u merge",
                     groups[iA].name, (unsigned)groups[iA].port);

            sendto(sock_ctrl, ctrl, strlen(ctrl), 0,
                   (struct sockaddr*)&groups[iB].addr, sizeof groups[iB].addr);

            // Message "visible" à tous les groupes pour informer de l’action
            char sysmsg[512];
            snprintf(sysmsg, sizeof sysmsg, "SYS [Fusion] %s a fusionne %s -> %s",
                     user, groups[iB].name, groups[iA].name);
            broadcast_to_groups(sysmsg);

            // Réponse au client demandeur
            char out[256];
            snprintf(out, sizeof out, "OK MERGE %s %s", groups[iA].name, groups[iB].name);
            sendto(sock_ctrl,out,strlen(out),0,(struct sockaddr*)&cli,cl);
            continue;
        }

        /* ───────── Commande inconnue ───────── */
        {
            const char *err="ERR unknown_cmd";
            sendto(sock_ctrl,err,strlen(err),0,(struct sockaddr*)&cli,cl);
        }
    }

    /* ───────────────────────── Arrêt propre ───────────────────────── */
    fprintf(stderr,"[Serveur] arrêt…\n");

    // Stop thread admin proprement
    pthread_cancel(th_in);
    pthread_join(th_in, NULL);

    // Tuer tous les groupes encore actifs
    for(unsigned i=0;i<GMAX;i++){
        if(groups[i].used){
            kill(groups[i].pid, SIGINT);
        }
    }

    // Attendre la fin des processus enfants
    for(unsigned i=0;i<GMAX;i++){
        if(groups[i].used){
            int st;
            waitpid(groups[i].pid, &st, 0);
        }
    }

    // Libération ressources
    if(sock_ctrl>=0) close(sock_ctrl);
    free(groups);
    return 0;
}
