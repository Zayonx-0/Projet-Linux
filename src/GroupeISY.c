// src/GroupeISY.c
#include "Commun.h"

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
    struct timeval tv; tv.tv_sec=ms/1000; tv.tv_usec=(ms%1000)*1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
static inline void send_txt(int s, const char *txt, const struct sockaddr_in *to){
    (void)sendto(s, txt, strlen(txt), 0, (const struct sockaddr*)to, sizeof *to);
}

int main(int argc, char **argv){
    if(argc<3){ fprintf(stderr, "Usage: %s <groupName> <port>\n", argv[0]); return 1; }
    const char *gname = argv[1];
    uint16_t gport    = (uint16_t)atoi(argv[2]);

    struct sigaction sa; memset(&sa,0,sizeof sa);
    sa.sa_handler = on_sigint; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    int s = socket(AF_INET, SOCK_DGRAM, 0); if(s<0) die_perror("socket");
    int yes=1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    set_rcv_timeout(s, 500);

    struct sockaddr_in addr={0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(gport);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(s,(struct sockaddr*)&addr,sizeof addr)<0) die_perror("bind group");

    Member m[MAX_MEMBERS]; memset(m,0,sizeof m);

    /* État de bannière du groupe */
    int banner_active = 0;
    char banner[TXT_LEN]; banner[0]='\0';

    fprintf(stdout, "GroupeISY '%s' lancé sur %u\n", gname, (unsigned)gport);

    char buf[512];
    while(running){
        struct sockaddr_in cli; socklen_t cl=sizeof cli;
        ssize_t n = recvfrom(s, buf, sizeof buf -1, 0, (struct sockaddr*)&cli, &cl);
        if(n<0){
            if(errno==EINTR) continue;
            if(errno==EAGAIN || errno==EWOULDBLOCK) continue;
            die_perror("recvfrom");
        }
        buf[n]='\0';

        if(!strncmp(buf, "MSG ", 4)){
            char user[EME_LEN]={0}; char *p = buf+4;
            if(sscanf(p, "%19s", user) != 1) continue;

            char *uend = strchr(p, ' ');
            if(!uend) continue;
            char *text = uend + 1;
            if(!*text) continue;

            int idx=-1; int is_new = 0;
            for(int i=0;i<MAX_MEMBERS;i++) if(m[i].inuse && !strcmp(m[i].user,user)){ idx=i; break; }
            if(idx<0){
                for(int i=0;i<MAX_MEMBERS;i++) if(!m[i].inuse){ idx=i; strncpy(m[i].user,user,EME_LEN-1); m[i].inuse=1; is_new=1; break; }
            }
            if(idx<0) continue;
            m[idx].addr = cli;
            if(m[idx].banned) continue;

            // Si nouveau et bannière active → renvoyer l'état immédiatement
            if(is_new && banner_active){
                char ctrl[512];
                snprintf(ctrl, sizeof ctrl, "CTRL BANNER_SET %s", banner);
                send_txt(s, ctrl, &cli);
            }

            char out[256]; snprintf(out,sizeof out,"GROUPE: Message de %s : %s", user, text);
            send_txt(s, out, &cli);
            for(int i=0;i<MAX_MEMBERS;i++){
                if(!m[i].inuse || m[i].banned) continue;
                if(m[i].addr.sin_addr.s_addr==cli.sin_addr.s_addr && m[i].addr.sin_port==cli.sin_port) continue;
                send_txt(s, out, &m[i].addr);
            }

        } else if(!strncmp(buf, "CMD ", 4)){
            if(!strncmp(buf+4, "LIST", 4)){
                char out[1024]={0};
                for(int i=0;i<MAX_MEMBERS;i++) if(m[i].inuse && !m[i].banned){
                    strncat(out, m[i].user, sizeof out - strlen(out) - 1);
                    strncat(out, "\n", sizeof out - strlen(out) - 1);
                }
                if(out[0]=='\0') strcpy(out,"(aucun)\n");
                send_txt(s,out,&cli);

            } else if(!strncmp(buf+4, "DELETE ", 7)){
                char who[EME_LEN]={0}; sscanf(buf+11, "%19s", who);
                int done=0; for(int i=0;i<MAX_MEMBERS;i++) if(m[i].inuse && !strcmp(m[i].user,who)){ m[i].banned=1; done=1; }
                char out[128]; snprintf(out,sizeof out, done?"Action: %s banni\n":"ERR: inconnu\n", who);
                send_txt(s,out,&cli);
            }

        } else if(!strncmp(buf, "SYS ", 4)){
            const char *text = buf + 4;
            if(*text == '\0') continue;
            char out[256]; snprintf(out,sizeof out,"Message de [SERVER] : %s", text);
            for(int i=0;i<MAX_MEMBERS;i++) if(m[i].inuse && !m[i].banned) send_txt(s,out,&m[i].addr);

        } else if(!strncmp(buf, "CTRL ", 5)){
            // Met à jour l'état, puis relaye tel quel
            if(!strncmp(buf, "CTRL BANNER_SET ", 16)){
                const char *t = buf + 16;
                strncpy(banner, t, TXT_LEN-1);
                banner[TXT_LEN-1]='\0';
                banner_active = 1;
            } else if(!strcmp(buf, "CTRL BANNER_CLR")){
                banner_active = 0;
                banner[0]='\0';
            }
            for(int i=0;i<MAX_MEMBERS;i++) if(m[i].inuse && !m[i].banned) send_txt(s,buf,&m[i].addr);
        }
    }

    close(s);
    fprintf(stdout, "GroupeISY '%s' arrêt.\n", gname);
    return 0;
}
