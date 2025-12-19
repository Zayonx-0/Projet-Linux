// src/AffichageISY.c
#include "Commun.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>

#define MAX_LOG_LINES 200
#define MAX_LINE      768

static volatile sig_atomic_t running = 1;

static void on_sig(int s){
    (void)s;
    running = 0;
}

typedef struct {
    int joined;
    char user[EME_LEN];
    char group[32];

    int admin_banner_active;
    char admin_banner[TXT_LEN];

    int idle_banner_active;
    char idle_banner[TXT_LEN];

    char log[MAX_LOG_LINES][MAX_LINE];
    int  log_count;

    int ui_dirty;
} UIState;

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

static void add_log(UIState *u, const char *line){
    if(!line || !*line) return;

    if(u->log_count < MAX_LOG_LINES){
        isy_strcpy(u->log[u->log_count], sizeof u->log[u->log_count], line);
        u->log_count++;
    }else{
        for(int i=1;i<MAX_LOG_LINES;i++){
            strcpy(u->log[i-1], u->log[i]);
        }
        isy_strcpy(u->log[MAX_LOG_LINES-1], sizeof u->log[MAX_LOG_LINES-1], line);
    }
    u->ui_dirty = 1;
}

static void redraw(UIState *u, int show_prompt){
    printf("\033[2J\033[H"); // clear + home

    int w = term_width();

    if(u->admin_banner_active){
        printf("=== BANNIERE ADMIN (SERVEUR) ===\n");
        wrap_print(u->admin_banner, w);
        printf("\n");
    }
    if(u->idle_banner_active){
        printf("=== BANNIERE INACTIVITE ===\n");
        wrap_print(u->idle_banner, w);
        printf("\n");
    }

    if(u->joined){
        printf("=== GROUPE: %s | USER: %s ===\n\n", u->group, u->user);
    }else{
        printf("=== PAS DANS UN GROUPE | USER: %s ===\n\n", u->user);
    }

    for(int i=0;i<u->log_count;i++){
        puts(u->log[i]);
    }

    if(show_prompt){
        if(u->joined) printf("\nMessage [%s] : ", u->group);
        else          printf("\n> ");
        fflush(stdout);
    }

    u->ui_dirty = 0;
}

/*
  Protocole UI (FIFO Client->Affichage), 1 ligne = 1 event :

  UI HEADER <joined:0|1> <user> <group>
  UI LOG <texte...>
  UI CLRLOG
  UI BANNER_ADMIN_SET <texte...>
  UI BANNER_ADMIN_CLR
  UI BANNER_IDLE_SET <texte...>
  UI BANNER_IDLE_CLR
  UI REDRAW
  UI QUIT

  Remarque: <texte...> = reste de ligne (peut contenir espaces)
*/

static void handle_ui_line(UIState *u, const char *line){
    if(!line) return;

    if(!strncmp(line, "UI HEADER ", 10)){
        int joined = 0;
        char user[EME_LEN] = {0};
        char group[32] = {0};
        // UI HEADER <0|1> <user> <group>
        if(sscanf(line + 10, "%d %19s %31s", &joined, user, group) >= 2){
            u->joined = joined ? 1 : 0;
            isy_strcpy(u->user, sizeof u->user, user);
            if(group[0]) isy_strcpy(u->group, sizeof u->group, group);
            else u->group[0] = '\0';
            u->ui_dirty = 1;
        }
        return;
    }

    if(!strncmp(line, "UI LOG ", 7)){
        add_log(u, line + 7);
        return;
    }

    if(!strcmp(line, "UI CLRLOG")){
        u->log_count = 0;
        u->ui_dirty = 1;
        return;
    }

    if(!strncmp(line, "UI BANNER_ADMIN_SET ", 20)){
        isy_strcpy(u->admin_banner, sizeof u->admin_banner, line + 20);
        u->admin_banner_active = 1;
        u->ui_dirty = 1;
        return;
    }
    if(!strcmp(line, "UI BANNER_ADMIN_CLR")){
        u->admin_banner_active = 0;
        u->admin_banner[0] = '\0';
        u->ui_dirty = 1;
        return;
    }

    if(!strncmp(line, "UI BANNER_IDLE_SET ", 19)){
        isy_strcpy(u->idle_banner, sizeof u->idle_banner, line + 19);
        u->idle_banner_active = 1;
        u->ui_dirty = 1;
        return;
    }
    if(!strcmp(line, "UI BANNER_IDLE_CLR")){
        u->idle_banner_active = 0;
        u->idle_banner[0] = '\0';
        u->ui_dirty = 1;
        return;
    }

    if(!strcmp(line, "UI REDRAW")){
        u->ui_dirty = 1;
        return;
    }

    if(!strcmp(line, "UI QUIT")){
        running = 0;
        return;
    }

    // fallback: log brute
    add_log(u, line);
}

static int set_nonblock(int fd){
    int fl = fcntl(fd, F_GETFL, 0);
    if(fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int main(int argc, char **argv){
    if(argc < 3){
        fprintf(stderr, "Usage: %s <fifo_in_from_client> <fifo_out_to_client>\n", argv[0]);
        return 1;
    }

    const char *fifo_in  = argv[1]; // Client -> UI
    const char *fifo_out = argv[2]; // UI -> Client

    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);

    // Ouvrir FIFO IN (lecture)
    int fd_in = open(fifo_in, O_RDONLY);
    if(fd_in < 0){
        perror("AffichageISY open fifo_in");
        return 1;
    }

    // Ouvrir FIFO OUT (écriture)
    int fd_out = open(fifo_out, O_WRONLY);
    if(fd_out < 0){
        perror("AffichageISY open fifo_out");
        close(fd_in);
        return 1;
    }

    set_nonblock(fd_in);

    UIState u;
    memset(&u, 0, sizeof u);
    isy_strcpy(u.user, sizeof u.user, "user");
    u.group[0] = '\0';
    u.joined = 0;
    u.ui_dirty = 1;

    char rbuf[2048];
    char linebuf[4096];
    size_t line_len = 0;

    while(running){
        // redraw if needed
        if(u.ui_dirty){
            redraw(&u, 1);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd_in, &rfds);
        FD_SET(STDIN_FILENO, &rfds);

        int maxfd = fd_in > STDIN_FILENO ? fd_in : STDIN_FILENO;

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if(r < 0){
            if(errno == EINTR) continue;
            break;
        }
        if(r == 0){
            continue;
        }

        // Events UI depuis le client
        if(FD_ISSET(fd_in, &rfds)){
            ssize_t n = read(fd_in, rbuf, sizeof rbuf);
            if(n > 0){
                for(ssize_t i=0;i<n;i++){
                    char c = rbuf[i];
                    if(c == '\n'){
                        linebuf[line_len] = '\0';
                        trimnl(linebuf);
                        if(linebuf[0]) handle_ui_line(&u, linebuf);
                        line_len = 0;
                    }else{
                        if(line_len + 1 < sizeof linebuf){
                            linebuf[line_len++] = c;
                        }
                    }
                }
            }
        }

        // Input clavier utilisateur -> transmettre au client
        if(FD_ISSET(STDIN_FILENO, &rfds)){
            char in[512];
            if(fgets(in, sizeof in, stdin)){
                trimnl(in);
                // renvoyer la ligne telle quelle (le client décide si cmd/msg/quit/etc.)
                if(in[0]){
                    dprintf(fd_out, "%s\n", in);
                }else{
                    // ligne vide : ne rien faire
                }
            }else{
                // EOF stdin
                running = 0;
            }
        }
    }

    // prévenir client (optionnel)
    dprintf(fd_out, "quit\n");

    close(fd_out);
    close(fd_in);
    return 0;
}
