#include "Commun.h"

#define MAX_MEMBERS 64

typedef struct {
    char user[EME_LEN];
    struct sockaddr_in addr;
    int banned;
    int inuse;
} Member;

static volatile sig_atomic_t running = 1;
static void on_sigint(int){ running = 0; }

static void send_txt(int s, const char *txt, const struct sockaddr_in *to){
    sendto(s, txt, strlen(txt), 0, (const struct sockaddr*)to, sizeof *to);
}

int main(int argc, char **argv){
    if(argc<3){ fprintf(stderr, "Usage: %s <groupName> <port>\n", argv[0]); return 1; }
    const char *gname = argv[1]; uint16_t gport = (uint16_t)atoi(argv[2]);
    signal(SIGINT, on_sigint);

    int s = socket(AF_INET, SOCK_DGRAM, 0); if(s<0) die_perror("socket");
    // (optionnel) SO_REUSEADDR pour dev intensif
    int yes=1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr={0};
    addr.sin_family=AF_INET;
    addr.sin_port=htons(gport);
    addr.sin_addr.s_addr=htonl(INADDR_ANY);
    if(bind(s,(struct sockaddr*)&addr,sizeof addr)<0) die_perror("bind group");

    Member m[MAX_MEMBERS]; memset(m,0,sizeof m);

    fprintf(stdout, "GroupeISY '%s' lancé sur %u\n", gname, (unsigned)gport);

    char buf[512];
    while(running){
        struct sockaddr_in cli; socklen_t cl=sizeof cli;
        ssize_t n = recvfrom(s, buf, sizeof buf -1, 0, (struct sockaddr*)&cli, &cl);
        if(n<0){ if(errno==EINTR) continue; die_perror("recvfrom"); }
        buf[n]='\0';

        // ── MSG <user> <texte...>
        if(!strncmp(buf, "MSG ", 4)){
            char user[EME_LEN]={0}; char *p = buf+4;
            if(sscanf(p, "%19s", user) != 1) continue;

            // trouver le début du texte : sauter "MSG " + <user> + ' '
            char *uend = strchr(p, ' ');
            if(!uend) continue;
            char *text = uend + 1;
            if(!*text) continue;

            // (re)enregistrer le membre à chaque message
            int idx=-1;
            for(int i=0;i<MAX_MEMBERS;i++) if(m[i].inuse && !strcmp(m[i].user,user)) { idx=i; break; }
            if(idx<0){
                for(int i=0;i<MAX_MEMBERS;i++) if(!m[i].inuse){ idx=i; strncpy(m[i].user,user,EME_LEN-1); m[i].inuse=1; break; }
            }
            if(idx<0) continue; // pas de slot
            m[idx].addr = cli;   // MAJ de l'adresse/port d’écoute
            if(m[idx].banned) continue;

            // 1) ACK immédiat au sender (garantit un affichage même si seul)
            char ack[256];
            snprintf(ack,sizeof ack,"Message de %s : %s\n", user, text);
            send_txt(s, ack, &cli);

            // 2) Broadcast aux autres membres valides
            for(int i=0;i<MAX_MEMBERS;i++){
                if(!m[i].inuse || m[i].banned) continue;
                // déjà envoyé au sender ci-dessus → on évite de le renvoyer
                if(m[i].addr.sin_addr.s_addr==cli.sin_addr.s_addr && m[i].addr.sin_port==cli.sin_port) continue;
                send_txt(s, ack, &m[i].addr);
            }

        // ── CMD LIST | CMD DELETE <user>
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

            } else {
                const char *e="ERR CMD\n"; send_txt(s,e,&cli);
            }

        // ── SYS <texte...>  (bannières serveur)
        } else if(!strncmp(buf, "SYS ", 4)){
            const char *text = buf + 4;
            if(*text == '\0') continue;
            char out[256]; snprintf(out,sizeof out,"Message de [SERVER] : %s\n", text);
            for(int i=0;i<MAX_MEMBERS;i++) if(m[i].inuse && !m[i].banned) send_txt(s,out,&m[i].addr);
        }
    }

    close(s);
    fprintf(stdout, "GroupeISY '%s' arrêt.\n", gname);
    return 0;
}
