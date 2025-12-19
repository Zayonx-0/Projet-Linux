// src/AffichageISY.c
#include "Commun.h"

#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

/*
    ─────────────────────────────────────────────────────────────────────────
    AffichageISY
    ─────────────────────────────────────────────────────────────────────────
    Rôle :
      - Ce programme gère UNIQUEMENT l'affichage (UI) côté client.
      - Il ne parle pas au réseau : il communique avec ClientISY via 2 FIFOs :
          * fifo_in  : ClientISY -> AffichageISY  (événements UI à afficher)
          * fifo_out : AffichageISY -> ClientISY  (entrées clavier de l'utilisateur)

    Objectif :
      - Garder les bannières (admin + inactivité) "pinnées" en haut du terminal.
      - Afficher l'historique des messages en bas, avec un "scroll" naturel :
        si l'historique dépasse la hauteur, on n'affiche que les dernières lignes.
      - Redessiner proprement lors d'un resize (SIGWINCH).
*/

#define MAX_LOG_LINES 800   // taille max de l'historique conservé
#define MAX_LINE      1024  // taille max d'une ligne UI

/*
    Etat UI:
      - joined : 1 si le client est dans un groupe, sinon 0
      - user/group : affichés dans le header
      - admin_banner / idle_banner : bannières "pinnées"
      - log[] : historique des messages à afficher
      - dirty : 1 => nécessite redraw
      - quit  : 1 => boucle principale se termine
*/
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

/* Flag mis à 1 quand le terminal est redimensionné */
static volatile sig_atomic_t g_winch = 0;

/* Handler SIGWINCH : on demande un redraw */
static void on_winch(int s){
    (void)s;
    g_winch = 1;
}

/* Largeur terminal (colonnes) */
static int term_width(void){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0){
        if(ws.ws_col > 10) return (int)ws.ws_col;
    }
    return 80;
}

/* Hauteur terminal (lignes) */
static int term_height(void){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0){
        if(ws.ws_row > 8) return (int)ws.ws_row;
    }
    return 24;
}

/*
    Renvoie le nombre de "lignes écran" consommées si on affiche s
    avec un wrap à width.

    Exemple :
      - width = 10
      - "HelloWorld123" => 2 lignes (HelloWorld + 123)
*/
static int wrapped_line_count(const char *s, int width){
    if(!s || !*s) return 1;
    if(width < 10) width = 10;

    int lines = 1;
    int col = 0;

    for(const char *p = s; *p; p++){
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

/*
    Affiche s avec wrap à width
    et renvoie le nombre de lignes consommées.

    Remarque :
      - On force un '\n' final si la dernière ligne n'en contient pas.
*/
static int wrap_print(const char *s, int width){
    if(!s || !*s){
        putchar('\n');
        return 1;
    }
    if(width < 10) width = 10;

    int lines = 1;
    int col = 0;

    for(const char *p = s; *p; p++){
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

/*
    Ajoute une ligne au log.
    Si le log est plein, on "scroll" (on décale tout) et on met à la fin.
*/
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

/* Efface l'historique */
static void clear_log(UIState *st){
    st->log_count = 0;
    st->dirty = 1;
}

/*
    redraw():
      - Clear écran + curseur en haut
      - Affiche les bannières "pinnées"
      - Affiche un header (groupe / user)
      - Affiche une fenêtre de log (les dernières lignes qui tiennent)
      - Affiche un prompt minimal en bas

    But :
      - Même si le log est énorme, les bannières restent visibles en haut.
*/
static void redraw(UIState *st){
    int w = term_width();
    int h = term_height();

    if(w < 20) w = 20;
    if(h < 10) h = 10;

    // Clear + home (ANSI escape)
    printf("\033[2J\033[H");

    int used_lines = 0;

    /* ───────── Bannières ───────── */
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

    /* ───────── Header ───────── */
    if(st->joined){
        printf("=== GROUPE: %s | USER: %s ===\n\n", st->group, st->user);
    }else{
        printf("=== PAS DANS UN GROUPE | USER: %s ===\n\n", st->user);
    }
    used_lines += 2; // header + ligne vide

    /*
        Calcul du nombre de lignes restantes pour le log.
        On réserve 2 lignes pour un prompt minimal.
    */
    int reserved_for_prompt = 2;
    int avail = h - used_lines - reserved_for_prompt;
    if(avail < 1) avail = 1;

    /*
        On veut afficher la fin du log :
          - on remonte depuis la dernière ligne
          - on cumule le nombre de lignes écran consommées (avec wrap)
          - on s'arrête dès qu'on dépasse avail
    */
    int start = st->log_count;
    int acc = 0;

    while(start > 0){
        const char *ln = st->log[start - 1];
        int need = wrapped_line_count(ln, w);

        if(acc + need > avail) break;

        acc += need;
        start--;
    }

    /*
        Cas extrême :
          - si la dernière ligne ne tient même pas (trop longue),
            on l'affiche quand même seule.
    */
    if(start == st->log_count && st->log_count > 0){
        start = st->log_count - 1;
    }

    /* ───────── Affichage du log ───────── */
    for(int i = start; i < st->log_count; i++){
        wrap_print(st->log[i], w);
    }

    /* ───────── Prompt ───────── */
    printf("\n> ");
    fflush(stdout);

    st->dirty = 0;
}

/*
    Lecture robuste sur fifo_in.
    On lit des octets, puis on découpe sur '\n'.

    Retour :
      1  => une ligne est disponible dans out
      0  => rien de dispo pour l'instant (EAGAIN)
     -1  => erreur ou EOF (fifo fermé)
*/
static int fifo_readline(int fd, char *out, size_t outsz){
    static char buf[8192];
    static size_t len = 0;

    for(;;){
        // Cherche un '\n' déjà dans le buffer
        for(size_t i = 0; i < len; i++){
            if(buf[i] == '\n'){
                size_t L = (i < outsz - 1) ? i : outsz - 1;

                memcpy(out, buf, L);
                out[L] = '\0';

                // Retire la ligne du buffer
                memmove(buf, buf + i + 1, len - (i + 1));
                len -= (i + 1);

                trimnl(out);
                return 1;
            }
        }

        // Sinon, on lit plus de données
        ssize_t n = read(fd, buf + len, sizeof(buf) - len);
        if(n < 0){
            if(errno == EINTR) continue;
            if(errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if(n == 0){
            return -1; // EOF
        }

        len += (size_t)n;

        // Sécurité : si la FIFO envoie un flux sans '\n' très long, on reset
        if(len >= sizeof(buf)){
            len = 0;
        }
    }
}

/*
    handle_ui_event():
      - parse les événements reçus depuis ClientISY via fifo_in
      - met à jour l'état UI
      - déclenche un redraw via st->dirty = 1
*/
static void handle_ui_event(UIState *st, const char *line){
    if(!line || !*line) return;

    // UI QUIT : demande de quitter l'UI
    if(!strcmp(line, "UI QUIT")){
        st->quit = 1;
        return;
    }

    // UI CLRLOG : efface l'historique
    if(!strcmp(line, "UI CLRLOG")){
        clear_log(st);
        return;
    }

    /*
        UI HEADER <joined> <user> <group>
        Exemple :
          UI HEADER 1 sophie ISEN
          UI HEADER 0 sophie -
    */
    if(!strncmp(line, "UI HEADER ", 10)){
        int joined = 0;
        char user[EME_LEN] = {0};
        char grp[32] = {0};

        if(sscanf(line + 10, "%d %19s %31s", &joined, user, grp) >= 2){
            st->joined = joined ? 1 : 0;
            isy_strcpy(st->user, sizeof st->user, user);

            if(grp[0] && strcmp(grp, "-") != 0){
                isy_strcpy(st->group, sizeof st->group, grp);
            }else{
                st->group[0] = '\0';
            }
            st->dirty = 1;
        }
        return;
    }

    // UI LOG <txt...> : ajoute une ligne au log
    if(!strncmp(line, "UI LOG ", 7)){
        add_log(st, line + 7);
        return;
    }

    /* ───────── Bannières ───────── */

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

    // UI REDRAW : force un redraw
    if(!strcmp(line, "UI REDRAW")){
        st->dirty = 1;
        return;
    }

    // Event inconnu : on l'affiche en log (utile pour debug)
    add_log(st, line);
}

int main(int argc, char **argv){
    if(argc < 3){
        fprintf(stderr, "Usage: %s <fifo_in> <fifo_out>\n", argv[0]);
        return 1;
    }

    const char *fifo_in_path  = argv[1]; // Client -> UI (read)
    const char *fifo_out_path = argv[2]; // UI -> Client (write)

    // Signaux : resize terminal
    signal(SIGWINCH, on_winch);

    /*
        Ouverture FIFO IN :
          - O_NONBLOCK : l'UI ne bloque pas si pas d'events
    */
    int fd_in = open(fifo_in_path, O_RDONLY | O_NONBLOCK);
    if(fd_in < 0) die_perror("AffichageISY open fifo_in");

    /*
        Ouverture FIFO OUT :
          - O_WRONLY : l'UI écrit les entrées utilisateur
          - Si ClientISY n'a pas ouvert le FIFO en lecture, open() peut bloquer :
            en pratique ClientISY ouvre d'abord l'autre extrémité.
    */
    int fd_out = open(fifo_out_path, O_WRONLY);
    if(fd_out < 0) die_perror("AffichageISY open fifo_out");

    UIState st;
    memset(&st, 0, sizeof st);

    isy_strcpy(st.user, sizeof st.user, "user");
    st.dirty = 1;

    // Premier rendu
    redraw(&st);

    char line[MAX_LINE];

    while(!st.quit){
        // Resize détecté ?
        if(g_winch){
            g_winch = 0;
            st.dirty = 1;
        }

        // Rendu si nécessaire
        if(st.dirty){
            redraw(&st);
        }

        // On attend soit un event UI (fifo_in), soit une entrée clavier (stdin)
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd_in, &rfds);
        FD_SET(STDIN_FILENO, &rfds);

        int maxfd = (fd_in > STDIN_FILENO) ? fd_in : STDIN_FILENO;

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 250000; // 250ms

        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if(r < 0){
            if(errno == EINTR) continue;
            break;
        }

        /* ───────── Events venant du client ───────── */
        if(FD_ISSET(fd_in, &rfds)){
            for(;;){
                int rr = fifo_readline(fd_in, line, sizeof line);

                if(rr == 1){
                    handle_ui_event(&st, line);
                    continue;
                }
                if(rr == 0){
                    break; // plus rien de dispo pour l'instant
                }

                // rr < 0 : fifo fermé => quitter
                st.quit = 1;
                break;
            }
        }

        /* ───────── Entrée utilisateur (clavier) ───────── */
        if(FD_ISSET(STDIN_FILENO, &rfds)){
            char inbuf[512];

            if(fgets(inbuf, sizeof inbuf, stdin)){
                trimnl(inbuf);

                // On renvoie tel quel au client (même vide)
                dprintf(fd_out, "%s\n", inbuf);
            }else{
                // stdin fermé => quitter l'UI
                st.quit = 1;
            }
        }
    }

    close(fd_in);
    close(fd_out);
    return 0;
}
