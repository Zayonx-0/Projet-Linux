// src/AffichageISY.c
#include "Commun.h"
#include <sys/stat.h>
#include <limits.h>

static void usage(const char *p){ fprintf(stderr, "Usage: %s <shm_or_file_spec> <sem_name>\n", p); }
static int starts_with(const char *s, const char *p){ return strncmp(s,p,strlen(p))==0; }

/* ───────── ANSI helpers ─────────
   On “épingle” la ligne 1 pour la bannière via la région de défilement:
   - activer:  \033[2;r   (scroll de la ligne 2 au bas)
   - désactiver: \033[r    (restaure scroll complet)
*/
static void ansi_full_clear(void){ printf("\033[2J\033[H"); fflush(stdout); }
static void ansi_scroll_region_banner_on(void){ printf("\033[2;r\033[H"); fflush(stdout); }
static void ansi_scroll_region_off(void){ printf("\033[r"); fflush(stdout); }  // restaurer
static void ansi_draw_banner_line(const char *txt){
    // Place curseur en (1,1), efface ligne 1, imprime en inverse vidéo + retour à la ligne
    printf("\033[H\033[2K\033[7m[BANNIERE] %s\033[0m\n", txt);
    fflush(stdout);
}

/* Historique d'affichage local (pour rerender lors du SET/CLR uniquement) */
#define MAX_LINES 1000
static char lines_buf[MAX_LINES][TXT_LEN];
static int  lines_cnt = 0;

static void push_line(const char *s){
    if(lines_cnt < MAX_LINES){
        strncpy(lines_buf[lines_cnt], s, TXT_LEN-1);
        lines_buf[lines_cnt][TXT_LEN-1] = '\0';
        lines_cnt++;
    }else{
        memmove(lines_buf, lines_buf+1, (MAX_LINES-1)*TXT_LEN);
        strncpy(lines_buf[MAX_LINES-1], s, TXT_LEN-1);
        lines_buf[MAX_LINES-1][TXT_LEN-1] = '\0';
    }
}

static void rerender_all(int banner_active, const char *banner){
    ansi_full_clear();
    if(banner_active){
        ansi_scroll_region_banner_on();
        ansi_draw_banner_line(banner);
    }else{
        ansi_scroll_region_off();
    }
    for(int i=0;i<lines_cnt;i++) puts(lines_buf[i]);
    fflush(stdout);
}

/* Au lancement, déduire l'état de bannière depuis les derniers messages du ring */
static void infer_banner_from_ring(ISYRing *rb, int *banner_active, char banner[TXT_LEN]){
    *banner_active = 0; banner[0]='\0';
    // On analyse jusqu'à SHM_RING_CAP derniers messages
    unsigned long w = rb->widx;
    unsigned long start = (w > SHM_RING_CAP) ? (w - SHM_RING_CAP) : 0UL;
    for(unsigned long i = start; i < w; ++i){
        ISYMsg *m = &rb->ring[i % SHM_RING_CAP];
        const char *t = m->Texte;
        if(starts_with(t, "CTRL BANNER_SET ")){
            const char *msg = t + strlen("CTRL BANNER_SET ");
            strncpy(banner, msg, TXT_LEN-1);
            banner[TXT_LEN-1]='\0';
            *banner_active = 1;
        }else if(!strcmp(t, "CTRL BANNER_CLR")){
            *banner_active = 0;
            banner[0]='\0';
        }
    }
}

int main(int argc, char **argv){
    if(argc<3){ usage(argv[0]); return 1; }
    const char *spec = argv[1];     // "/shm_name" ou "file:/path"
    const char *sem_name = argv[2];

    ISYRing *rb = NULL;
    int fd = -1;

    if (strncmp(spec, "file:", 5) == 0) {
        const char *path = spec + 5;
        fd = open(path, O_RDWR);
        if(fd < 0) die_perror("open(file)");
        rb = mmap(NULL, sizeof(ISYRing), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if(rb == MAP_FAILED) die_perror("mmap(file)");
    } else {
        fd = shm_open(spec, O_RDWR, 0600);
        if(fd < 0) die_perror("shm_open");
        rb = mmap(NULL, sizeof(ISYRing), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if(rb == MAP_FAILED) die_perror("mmap(shm)");
    }

    sem_t *sem = sem_open(sem_name, 0);
    if(sem==SEM_FAILED) die_perror("sem_open");

    /* État bannière au démarrage (important pour re-entrée dialog) */
    int banner_active = 0;
    char banner[TXT_LEN]; banner[0]='\0';
    infer_banner_from_ring(rb, &banner_active, banner);

    // Premier rendu propre
    ansi_full_clear();
    if(banner_active){
        ansi_scroll_region_banner_on();
        ansi_draw_banner_line(banner);
    }else{
        ansi_scroll_region_off();
    }

    // (Optionnel) message d’attache visible une fois
    // printf("AffichageISY: attaché à %s\n", spec); fflush(stdout);

    while(1){
        sem_wait(sem);
        while (rb->ridx != rb->widx){
            ISYMsg *m = &rb->ring[rb->ridx % SHM_RING_CAP];
            const char *t = m->Texte;

            if(starts_with(t, "CTRL BANNER_SET ")){
                const char *msg = t + strlen("CTRL BANNER_SET ");
                strncpy(banner, msg, TXT_LEN-1);
                banner[TXT_LEN-1] = '\0';
                banner_active = 1;
                // Reconfig scroll + rerender historique
                rerender_all(banner_active, banner);

            } else if(!strcmp(t, "CTRL BANNER_CLR")){
                banner_active = 0;
                banner[0]='\0';
                rerender_all(banner_active, banner);

            } else {
                // Message normal (déjà au format "GROUPE: Message de X : Y")
                push_line(t);
                if(banner_active){
                    // Avec région de scroll active, la bannière (ligne 1) ne scrolle pas.
                    // On n'efface pas tout : on imprime juste la nouvelle ligne.
                    puts(t);
                    fflush(stdout);
                } else {
                    puts(t);
                    fflush(stdout);
                }
            }

            rb->ridx++;
        }
        if(rb->closed && rb->ridx==rb->widx) break;
    }

    // Reset terminal scroll region avant de quitter
    ansi_scroll_region_off();

    munmap(rb, sizeof *rb);
    if (fd >= 0) close(fd);
    sem_close(sem);
    return 0;
}
