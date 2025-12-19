// src/AffichageISY.c
#include "Commun.h"

#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#define MAX_LOG_LINES 800
#define MAX_LINE      1024

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

    int dirty;
    int quit;
} UIState;

static volatile sig_atomic_t g_winch = 0;

static void on_winch(int s){
    (void)s;
    g_winch = 1;
}

static int term_width(void){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0){
        if(ws.ws_col > 10) return (int)ws.ws_col;
    }
    return 80;
}

static int term_height(void){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0){
        if(ws.ws_row > 8) return (int)ws.ws_row;
    }
    return 24;
}

/* Renvoie le nombre de lignes "écran" consommées si on affiche s avec wrap à width. */
static int wrapped_line_count(const char *s, int width){
    if(!s || !*s) return 1;
    if(width < 10) width = 10;

    int lines = 1;
    int col = 0;
    for(const char *p=s; *p; p++){
        if(*p == '\n'){
            lines++;
            col = 0;
            continue;
        }
        col++;
        if(col >= width){
            lines++;
            col = 0;
        }
    }
    return lines;
}

/* Affiche s avec wrap à width et renvoie le nb de lignes consommées. */
static int wrap_print(const char *s, int width){
    if(!s || !*s){
        putchar('\n');
        return 1;
    }
    if(width < 10) width = 10;

    int lines = 1;
    int col = 0;
    for(const char *p=s; *p; p++){
        putchar(*p);
        if(*p == '\n'){
            lines++;
            col = 0;
            continue;
        }
        col++;
        if(col >= width){
            putchar('\n');
            lines++;
            col = 0;
        }
    }
    if(col != 0){
        putchar('\n');
    }
    return lines;
}

static void add_log(UIState *st, const char *line){
    if(!line) return;
    if(st->log_count < MAX_LOG_LINES){
        isy_strcpy(st->log[st->log_count], sizeof st->log[st->log_count], line);
        st->log_count++;
    }else{
        for(int i=1;i<MAX_LOG_LINES;i++){
            strcpy(st->log[i-1], st->log[i]);
        }
        isy_strcpy(st->log[MAX_LOG_LINES-1], sizeof st->log[MAX_LOG_LINES-1], line);
    }
    st->dirty = 1;
}

static void clear_log(UIState *st){
    st->log_count = 0;
    st->dirty = 1;
}

/*
  Rendu "pinné" :
  - On clear l'écran
  - On affiche bannières + header en haut
  - Puis on affiche uniquement la fin du log qui tient dans la hauteur restante
  - Puis un prompt en bas
*/
static void redraw(UIState *st){
    int w = term_width();
    int h = term_height();
    if(w < 20) w = 20;
    if(h < 10) h = 10;

    // Clear + home
    printf("\033[2J\033[H");

    int used_lines = 0;

    // Bannières
    if(st->admin_banner_active){
        printf("=== BANNIERE ADMIN (SERVEUR) ===\n");
        used_lines += 1;
        used_lines += wrap_print(st->admin_banner, w);
        putchar('\n');
        used_lines += 1;
    }
    if(st->idle_banner_active){
        printf("=== BANNIERE INACTIVITE ===\n");
        used_lines += 1;
        used_lines += wrap_print(st->idle_banner, w);
        putchar('\n');
        used_lines += 1;
    }

    // Header
    if(st->joined){
        printf("=== GROUPE: %s | USER: %s ===\n\n", st->group, st->user);
    }else{
        printf("=== PAS DANS UN GROUPE | USER: %s ===\n\n", st->user);
    }
    used_lines += 2; // header line + blank line

    // Lignes dispo pour log + prompt
    // On réserve 2 lignes pour le prompt (texte + input)
    int reserved_for_prompt = 2;
    int avail = h - used_lines - reserved_for_prompt;
    if(avail < 1) avail = 1;

    // Choisir une fenêtre de log à afficher (du bas vers le haut) en tenant compte du wrap.
    int start = st->log_count; // on va remonter
    int acc = 0;

    while(start > 0){
        const char *ln = st->log[start-1];
        int need = wrapped_line_count(ln, w);
        if(acc + need > avail) break;
        acc += need;
        start--;
    }

    // Si une seule ligne de log est énorme, on l'affiche quand même (start peut rester = log_count)
    if(start == st->log_count && st->log_count > 0){
        start = st->log_count - 1;
    }

    // Afficher la fenêtre [start..end[
    for(int i=start; i<st->log_count; i++){
        wrap_print(st->log[i], w);
    }

    // Prompt
    if(st->joined){
        printf("\n> ");
    }else{
        printf("\n> ");
    }
    fflush(stdout);

    st->dirty = 0;
}

/* Lecture robuste ligne par ligne depuis FIFO IN */
static int fifo_readline(int fd, char *out, size_t outsz){
    static char buf[8192];
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

        ssize_t n = read(fd, buf+len, sizeof(buf)-len);
        if(n < 0){
            if(errno == EINTR) continue;
            if(errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if(n == 0) return -1; // EOF
        len += (size_t)n;
        if(len >= sizeof(buf)) len = 0; // éviter overflow en cas de flux invalide
    }
}

/* Parse events UI */
static void handle_ui_event(UIState *st, const char *line){
    if(!line || !*line) return;

    // UI QUIT
    if(!strcmp(line, "UI QUIT")){
        st->quit = 1;
        return;
    }

    // UI CLRLOG
    if(!strcmp(line, "UI CLRLOG")){
        clear_log(st);
        return;
    }

    // UI HEADER <joined> <user> <group>
    if(!strncmp(line, "UI HEADER ", 10)){
        int joined = 0;
        char user[EME_LEN]={0};
        char grp[32]={0};
        if(sscanf(line+10, "%d %19s %31s", &joined, user, grp) >= 2){
            st->joined = joined ? 1 : 0;
            isy_strcpy(st->user, sizeof st->user, user);
            if(grp[0] && strcmp(grp,"-") != 0){
                isy_strcpy(st->group, sizeof st->group, grp);
            }else{
                st->group[0] = '\0';
            }
            st->dirty = 1;
        }
        return;
    }

    // UI LOG <txt...>
    if(!strncmp(line, "UI LOG ", 7)){
        add_log(st, line + 7);
        return;
    }

    // Banners
    if(!strncmp(line, "UI BANNER_ADMIN_SET ", 20)){
        st->admin_banner_active = 1;
        isy_strcpy(st->admin_banner, sizeof st->admin_banner, line + 20);
        st->dirty = 1;
        return;
    }
    if(!strcmp(line, "UI BANNER_ADMIN_CLR")){
        st->admin_banner_active = 0;
        st->admin_banner[0] = '\0';
        st->dirty = 1;
        return;
    }
    if(!strncmp(line, "UI BANNER_IDLE_SET ", 19)){
        st->idle_banner_active = 1;
        isy_strcpy(st->idle_banner, sizeof st->idle_banner, line + 19);
        st->dirty = 1;
        return;
    }
    if(!strcmp(line, "UI BANNER_IDLE_CLR")){
        st->idle_banner_active = 0;
        st->idle_banner[0] = '\0';
        st->dirty = 1;
        return;
    }

    // UI REDRAW
    if(!strcmp(line, "UI REDRAW")){
        st->dirty = 1;
        return;
    }

    // Event inconnu => log (pour debug)
    add_log(st, line);
}

int main(int argc, char **argv){
    if(argc < 3){
        fprintf(stderr, "Usage: %s <fifo_in> <fifo_out>\n", argv[0]);
        return 1;
    }

    const char *fifo_in_path  = argv[1]; // Client -> UI (read)
    const char *fifo_out_path = argv[2]; // UI -> Client (write)

    // signals
    signal(SIGWINCH, on_winch);

    int fd_in = open(fifo_in_path, O_RDONLY | O_NONBLOCK);
    if(fd_in < 0) die_perror("AffichageISY open fifo_in");

    int fd_out = open(fifo_out_path, O_WRONLY);
    if(fd_out < 0) die_perror("AffichageISY open fifo_out");

    UIState st;
    memset(&st, 0, sizeof st);
    isy_strcpy(st.user, sizeof st.user, "user");
    st.dirty = 1;

    // First draw
    redraw(&st);

    char line[MAX_LINE];

    while(!st.quit){
        if(g_winch){
            g_winch = 0;
            st.dirty = 1;
        }

        if(st.dirty){
            redraw(&st);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd_in, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = (fd_in > STDIN_FILENO) ? fd_in : STDIN_FILENO;

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 250000;

        int r = select(maxfd+1, &rfds, NULL, NULL, &tv);
        if(r < 0){
            if(errno == EINTR) continue;
            break;
        }

        // Events UI
        if(FD_ISSET(fd_in, &rfds)){
            for(;;){
                int rr = fifo_readline(fd_in, line, sizeof line);
                if(rr == 1){
                    handle_ui_event(&st, line);
                    continue;
                }
                if(rr == 0){
                    break; // plus rien pour l'instant
                }
                // rr < 0 : fifo fermé => quitter
                st.quit = 1;
                break;
            }
        }

        // Input utilisateur => renvoyer au client
        if(FD_ISSET(STDIN_FILENO, &rfds)){
            char inbuf[512];
            if(fgets(inbuf, sizeof inbuf, stdin)){
                trimnl(inbuf);
                // même si ligne vide, on la transmet (le client décidera quoi en faire)
                dprintf(fd_out, "%s\n", inbuf);
            }else{
                // stdin fermé => quitter
                st.quit = 1;
            }
        }
    }

    close(fd_in);
    close(fd_out);
    return 0;
}
