// src/ClientISY.c
#include "Commun.h"
#include <sys/stat.h>
#include <limits.h>
#include <sys/wait.h>

/* ───────── Config ───────── */
typedef struct {
    char user[EME_LEN];
    char srv_ip[64];
    uint16_t srv_port;
    char shm_prefix[64];   // ex: "/isy"
    uint16_t local_port;   // port UDP local pour recevoir les messages du groupe
} ClientConf;

static int load_conf(const char *path, ClientConf *c){
    memset(c, 0, sizeof *c);
    strncpy(c->user, "user", EME_LEN-1);
    strncpy(c->srv_ip, "127.0.0.1", sizeof c->srv_ip -1);
    strncpy(c->shm_prefix, "/isy", sizeof c->shm_prefix -1);
    c->srv_port = 8000;
    c->local_port = 9001;

    FILE *f=fopen(path,"r"); if(!f) return -1;
    char line[256], k[64], v[128];
    while(fgets(line,sizeof line,f)){
        if(line[0]=='#' || strlen(line)<3) continue;
        if(sscanf(line, "%63[^=]=%127s", k, v)==2){
            if(!strcmp(k,"USER")) strncpy(c->user, v, EME_LEN-1);
            else if(!strcmp(k,"SERVER_IP")) strncpy(c->srv_ip, v, sizeof c->srv_ip -1);
            else if(!strcmp(k,"SERVER_PORT")) c->srv_port=(uint16_t)atoi(v);
            else if(!strcmp(k,"SHM_PREFIX")) strncpy(c->shm_prefix, v, sizeof c->shm_prefix -1);
            else if(!strcmp(k,"LOCAL_RECV_PORT")) c->local_port=(uint16_t)atoi(v);
        }
    }
    fclose(f);
    return 0;
}

static void make_names(const char *prefix, const char *user, const char *group,
                       char *shm, size_t shmsz, char *sem, size_t semsz){
    if(prefix[0] == '/'){
        snprintf(shm, shmsz, "%s_%s_%s", prefix, user, group);
        snprintf(sem, semsz, "%s_sem_%s_%s", prefix, user, group);
    }else{
        snprintf(shm, shmsz, "/%s_%s_%s", prefix, user, group);
        snprintf(sem, semsz, "/%s_sem_%s_%s", prefix, user, group);
    }
}

static void menu(void){
    puts("Choix des commandes :            ");
    puts("0 Creation de groupe");
    puts("1 Rejoindre un groupe");
    puts("2 Lister les groupes");
    puts("3 Dialoguer sur un groupe");
    puts("4 Quitter");
    puts("5 Quitter le groupe");
}

/* ───────── RX thread ───────── */
typedef struct {
    int rx_sock;
    volatile int stop;
    ISYRing *rb;
    sem_t *sem;
} RxCtx;

static void set_recv_timeout(int sock, int ms){
    struct timeval tv;
    tv.tv_sec  = ms/1000;
    tv.tv_usec = (ms%1000)*1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static void *rx_thread(void *arg){
    RxCtx *ctx = (RxCtx*)arg;
    char buf[512];
    set_recv_timeout(ctx->rx_sock, 400);
    while(!ctx->stop){
        struct sockaddr_in from; socklen_t fl = sizeof from;
        ssize_t n = recvfrom(ctx->rx_sock, buf, sizeof buf - 1, 0, (struct sockaddr*)&from, &fl);
        if(n < 0){
            if(errno==EINTR) continue;
            if(errno==EAGAIN || errno==EWOULDBLOCK) continue;
            break;
        }
        buf[n]='\0';
        ISYMsg *m = &ctx->rb->ring[ctx->rb->widx % SHM_RING_CAP];
        strncpy(m->Ordre, "MES", ORDRE_LEN);
        strncpy(m->Emetteur, "GROUPE", EME_LEN);
        strncpy(m->Texte, buf, TXT_LEN-1);
        __sync_synchronize();
        ctx->rb->widx++;
        sem_post(ctx->sem);
    }
    return NULL;
}

/* ───────── Affichage (process séparé) ───────── */
static pid_t spawn_affichage(const char *spec, const char *sem_name){
    pid_t pid = fork();
    if(pid < 0) die_perror("fork AffichageISY");
    if(pid == 0){
        execl("./AffichageISY", "AffichageISY", spec, sem_name, (char*)NULL);
        _exit(127);
    }
    return pid;
}

/* ───────── UI helpers (éviter l’écho dans le terminal client) ───────── */
static inline void ui_prompt(void){
    /* Efface la ligne courante et réaffiche un prompt court, SANS retour à la ligne */
    printf("\033[2K\rMessage : ");
    fflush(stdout);
}
static inline void ui_clear_prevline(void){
    /* Remonte d'une ligne (celle où l'utilisateur vient de taper), efface, revient début */
    printf("\033[1A\033[2K\r");
    fflush(stdout);
}

/* ───────── Boucle de dialogue : Affichage dédié + effacement écho ───────── */
static void dialog_loop(const char *user, int rx, struct sockaddr_in *grp,
                        ISYRing *rb, sem_t *sem,
                        const char *display_spec, const char *sem_name){
    char line[256];

    /* (Ré)ouvre l’affichage uniquement pour la durée du dialogue */
    rb->closed = 0;
    pid_t viewer = spawn_affichage(display_spec, sem_name);

    puts("Tapez quit pour revenir au menu , cmd pour entrer une commande , msg pour revenir aux messages");
    ui_prompt();

    for(;;){
        if(!fgets(line, sizeof line, stdin)) break;
        trimnl(line);

        /* On efface la ligne que l'utilisateur vient d'envoyer */
        ui_clear_prevline();

        if(!strcmp(line, "quit")) break;

        if(!strcmp(line, "cmd")){
            /* prompt commande sur la même ligne */
            printf("Commande : ");
            fflush(stdout);
            if(!fgets(line, sizeof line, stdin)) break;
            trimnl(line);
            /* efface la ligne de commande saisie */
            ui_clear_prevline();

            if(!strncmp(line, "list", 4)){
                const char *cmdg = "CMD LIST";
                if(sendto(rx, cmdg, strlen(cmdg), 0, (struct sockaddr*)grp, sizeof *grp) < 0) die_perror("sendto CMD LIST");
            } else if(!strncmp(line, "Delete ", 7) || !strncmp(line, "DELETE ", 7)){
                char cmdg[128]; snprintf(cmdg, sizeof cmdg, "CMD DELETE %s", line+7);
                if(sendto(rx, cmdg, strlen(cmdg), 0, (struct sockaddr*)grp, sizeof *grp) < 0) die_perror("sendto CMD DELETE");
            } /* sinon: commande inconnue -> rien à afficher côté client */
        } else if(line[0] != '\0'){
            char out[256];
            snprintf(out, sizeof out, "MSG %s %s", user, line);
            if(sendto(rx, out, strlen(out), 0, (struct sockaddr*)grp, sizeof *grp) < 0) die_perror("sendto MSG");
        }

        /* Ré-affiche un prompt propre sur la même ligne (pas d’écho du texte tapé) */
        ui_prompt();
    }

    /* Arrêt propre de l’affichage local */
    rb->closed = 1;
    sem_post(sem);
    waitpid(viewer, NULL, 0);
}

/* ───────── Main ───────── */
int main(int argc, char **argv){
    if(argc<2){ fprintf(stderr, "Usage: %s conf/client.conf\n", argv[0]); return 1; }

    ClientConf conf;
    if(load_conf(argv[1], &conf) < 0) die_perror("client conf");

    // socket → serveur (LIST/CREATE/JOIN/QUIT)
    int s = socket(AF_INET, SOCK_DGRAM, 0); if(s<0) die_perror("socket srv");
    struct sockaddr_in srv; memset(&srv,0,sizeof srv);
    srv.sin_family = AF_INET; srv.sin_port = htons(conf.srv_port);
    if(inet_pton(AF_INET, conf.srv_ip, &srv.sin_addr) != 1) die_perror("inet_pton srv");

    // socket local réception/envoi vers groupe (bindé sur LOCAL_RECV_PORT)
    int rx = socket(AF_INET, SOCK_DGRAM, 0); if(rx<0) die_perror("socket rx");
    struct sockaddr_in laddr; memset(&laddr,0,sizeof laddr);
    laddr.sin_family = AF_INET; laddr.sin_port = htons(conf.local_port);
    laddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(rx, (struct sockaddr*)&laddr, sizeof laddr) < 0) die_perror("bind rx");

    /* État session */
    int joined = 0;
    char current_group[32] = "";
    struct sockaddr_in grp; memset(&grp,0,sizeof grp);
    char sem_name_saved[128]="";
    char display_spec_saved[PATH_MAX + 6]="";

    // Ressources session
    ISYRing *rb  = NULL;
    sem_t  *sem  = NULL;
    pthread_t rx_th;
    RxCtx *ctx = NULL;

    char line[256];

    for(;;){
        menu();
        printf("Choix : "); fflush(stdout);
        if(!fgets(line,sizeof line, stdin)) break; trimnl(line);

        if(!strcmp(line,"2")){
            const char *cmd = "LIST";
            if(sendto(s, cmd, strlen(cmd), 0, (struct sockaddr*)&srv, sizeof srv) < 0) die_perror("sendto LIST");
            char buf[1024]; struct sockaddr_in from; socklen_t fl=sizeof from;
            ssize_t n = recvfrom(s, buf, sizeof buf -1, 0, (struct sockaddr*)&from, &fl);
            if(n>0){ buf[n]='\0'; puts(buf); }
            continue;
        }

        if(!strcmp(line,"0")){
            printf("Saisire le nom du groupe\n");
            if(!fgets(line,sizeof line, stdin)) continue; trimnl(line);
            char cmd[128]; snprintf(cmd, sizeof cmd, "CREATE %s", line);
            if(sendto(s, cmd, strlen(cmd), 0, (struct sockaddr*)&srv, sizeof srv) < 0) die_perror("sendto CREATE");
            char buf[256]; struct sockaddr_in from; socklen_t fl=sizeof from;
            ssize_t n = recvfrom(s, buf, sizeof buf -1, 0, (struct sockaddr*)&from, &fl);
            if(n>0){ buf[n]='\0'; puts(buf); }
            continue;
        }

        if(!strcmp(line,"1")){
            if(joined){ puts("Vous êtes déjà dans un groupe. Utilisez 5 pour le quitter ou 3 pour dialoguer."); continue; }

            printf("Saisire le nom du groupe\n");
            char gname[32]; if(!fgets(gname,sizeof gname, stdin)) continue; trimnl(gname);

            char cip[64] = "127.0.0.1";
            char cmd[256];
            snprintf(cmd, sizeof cmd, "JOIN %s %s %s %u", gname, conf.user, cip, (unsigned)conf.local_port);
            if(sendto(s, cmd, strlen(cmd), 0, (struct sockaddr*)&srv, sizeof srv) < 0) die_perror("sendto JOIN");

            char buf[256]; struct sockaddr_in from; socklen_t fl=sizeof from;
            ssize_t n = recvfrom(s, buf, sizeof buf -1, 0, (struct sockaddr*)&from, &fl);
            if(n<=0){ puts("Join failed"); continue; }
            buf[n]='\0';

            unsigned gport = 0;
            if(sscanf(buf, "OK %31s %u", current_group, &gport) != 2){
                puts("ERR JOIN"); current_group[0]='\0'; continue;
            }

            grp.sin_family = AF_INET; grp.sin_port = htons((uint16_t)gport);
            if(inet_pton(AF_INET, "127.0.0.1", &grp.sin_addr) != 1) die_perror("inet_pton grp");

            /* SHM/SEM avec fallback fichier */
            char shm_name[128], sem_name[128];
            make_names(conf.shm_prefix, conf.user, current_group, shm_name, sizeof shm_name, sem_name, sizeof sem_name);
            strncpy(sem_name_saved, sem_name, sizeof sem_name_saved -1);

            shm_unlink(shm_name); sem_unlink(sem_name);

            int shm_fd = -1, use_file = 0;
            char file_spec[PATH_MAX]; file_spec[0]='\0';

            shm_fd = shm_open(shm_name, O_CREAT|O_EXCL|O_RDWR, 0600);
            if (shm_fd < 0 && errno == EEXIST) shm_fd = shm_open(shm_name, O_RDWR, 0600);

            if (shm_fd >= 0) {
                size_t need = sizeof(ISYRing);
                long pg = sysconf(_SC_PAGESIZE); if (pg < 1) pg = 4096;
                size_t aligned = (need + (pg - 1)) & ~(pg - 1);
                if (ftruncate(shm_fd, (off_t)aligned) < 0) {
                    close(shm_fd); shm_fd = -1;
                    shm_unlink(shm_name);
                    use_file = 1;
                }
            } else {
                use_file = 1;
            }

            if (use_file) {
                int ffd;
                snprintf(file_spec, sizeof file_spec, "/tmp/isy_%s_%s.ring", conf.user, current_group);
                ffd = open(file_spec, O_CREAT|O_RDWR, 0600);
                if (ffd < 0) die_perror("open(/tmp ring)");
                size_t need = sizeof(ISYRing);
                long pg = sysconf(_SC_PAGESIZE); if (pg < 1) pg = 4096;
                size_t aligned = (need + (pg - 1)) & ~(pg - 1);
                if (ftruncate(ffd, (off_t)aligned) < 0) die_perror("ftruncate(file)");
                rb = mmap(NULL, sizeof(ISYRing), PROT_READ|PROT_WRITE, MAP_SHARED, ffd, 0);
                if (rb == MAP_FAILED) die_perror("mmap(file)");
                memset(rb, 0, sizeof *rb);
                close(ffd);
                snprintf(display_spec_saved, sizeof display_spec_saved, "file:%s", file_spec);
            } else {
                rb = mmap(NULL, sizeof(ISYRing), PROT_READ|PROT_WRITE, MAP_SHARED, shm_fd, 0);
                if (rb == MAP_FAILED) die_perror("mmap(shm)");
                memset(rb, 0, sizeof *rb);
                close(shm_fd);
                strncpy(display_spec_saved, shm_name, sizeof display_spec_saved - 1);
                display_spec_saved[sizeof display_spec_saved -1] = '\0';
            }

            sem = sem_open(sem_name, O_CREAT|O_EXCL, 0600, 0);
            if(sem == SEM_FAILED && errno == EEXIST) sem = sem_open(sem_name, 0);
            if(sem == SEM_FAILED) die_perror("sem_open");

            /* Thread RX (persistant tant qu’on ne quitte pas le groupe) */
            ctx = (RxCtx*)calloc(1, sizeof *ctx);
            ctx->rx_sock = rx; ctx->stop = 0; ctx->rb = rb; ctx->sem = sem;
            if(pthread_create(&rx_th, NULL, rx_thread, ctx) != 0) die_perror("pthread_create");

            /* auto-enregistrement pour que le groupe nous “voit” sur rx */
            {
                char hello[128];
                snprintf(hello, sizeof hello, "MSG %s %s", conf.user, "(joined)");
                if(sendto(rx, hello, strlen(hello), 0, (struct sockaddr*)&grp, sizeof grp) < 0) die_perror("sendto hello");
            }

            joined = 1;
            dialog_loop(conf.user, rx, &grp, rb, sem, display_spec_saved, sem_name_saved);
            continue;
        }

        if(!strcmp(line,"3")){
            if(!joined){ puts("Rejoignez un groupe d'abord (option 1)."); continue; }
            dialog_loop(conf.user, rx, &grp, rb, sem, display_spec_saved, sem_name_saved);
            continue;
        }

        if(!strcmp(line,"5")){
            if(!joined){ puts("Vous n'êtes dans aucun groupe."); continue; }
            if(ctx){ ctx->stop = 1; pthread_join(rx_th, NULL); free(ctx); ctx = NULL; }
            if(rb){ rb->closed = 1; if(sem) sem_post(sem); munmap(rb, sizeof *rb); rb = NULL; }
            if(sem){ sem_close(sem); sem = NULL; }
            current_group[0] = '\0';
            sem_name_saved[0] = '\0';
            display_spec_saved[0] = '\0';
            joined = 0;
            puts("Groupe quitté.");
            continue;
        }

        if(!strcmp(line,"4")) break;
    }

    if(joined){
        if(ctx){ ctx->stop = 1; pthread_join(rx_th, NULL); free(ctx); }
        if(rb){ rb->closed = 1; if(sem) sem_post(sem); munmap(rb, sizeof *rb); }
        if(sem){ sem_close(sem); }
    }
    close(rx);
    close(s);
    return 0;
}
