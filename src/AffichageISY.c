// src/AffichageISY.c
#include "Commun.h"
#include <sys/stat.h>
#include <limits.h>

/*
   Deux bannières collantes :
     - CTRL BANNER_SET / CTRL BANNER_CLR  → Bannière ADMIN
     - CTRL IBANNER_SET / CTRL IBANNER_CLR → Bannière INACTIVITÉ

   Nouveau comportement :
     - Pas de troncature : le texte est affiché en entier, l’OS gère les retours à la ligne.
     - À chaque message (normal ou contrôle), on re-render :
         clear écran -> bannières actives -> historique complet.
*/

static void usage(const char *p){ fprintf(stderr, "Usage: %s <shm_or_file_spec> <sem_name>\n", p); }
static int starts_with(const char *s, const char *p){ return strncmp(s,p,strlen(p))==0; }

static void ansi_full_clear(void){
    printf("\033[2J\033[H");
    fflush(stdout);
}

/* ───────── Historique local ───────── */
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

/* ───────── Rendu complet ───────── */
static void rerender_all(int admin_on, const char *admin,
                         int idle_on,  const char *idle){
    ansi_full_clear();

    if(admin_on){
        printf("\033[7m[ADMIN] %s\033[0m\n", admin);
    }
    if(idle_on){
        printf("\033[7m[INACTIVITE] %s\033[0m\n", idle);
    }

    for(int i=0;i<lines_cnt;i++){
        puts(lines_buf[i]);
    }
    fflush(stdout);
}

/* ───────── Inférence des bannières au démarrage ───────── */
static void infer_banners_from_ring(ISYRing *rb,
                                    int *adm_on, char adm[TXT_LEN],
                                    int *idl_on, char idl[TXT_LEN]){
    *adm_on=0; *idl_on=0; adm[0]='\0'; idl[0]='\0';
    unsigned long w = rb->widx;
    unsigned long start = (w > SHM_RING_CAP ? w - SHM_RING_CAP : 0UL);
    for(unsigned long i=start; i<w; ++i){
        ISYMsg *m = &rb->ring[i % SHM_RING_CAP];
        const char *t = m->Texte;
        if(starts_with(t,"CTRL BANNER_SET ")){
            strncpy(adm, t+16, TXT_LEN-1); adm[TXT_LEN-1]='\0'; *adm_on=1;
        }else if(!strcmp(t,"CTRL BANNER_CLR")){
            *adm_on=0; adm[0]='\0';
        }else if(starts_with(t,"CTRL IBANNER_SET ")){
            strncpy(idl, t+18, TXT_LEN-1); idl[TXT_LEN-1]='\0'; *idl_on=1;
        }else if(!strcmp(t,"CTRL IBANNER_CLR")){
            *idl_on=0; idl[0]='\0';
        }
    }
}

/* ───────── Main ───────── */
int main(int argc, char **argv){
    if(argc<3){ usage(argv[0]); return 1; }
    const char *spec = argv[1];
    const char *sem_name = argv[2];

    ISYRing *rb=NULL;
    int fd=-1;

    if(strncmp(spec,"file:",5)==0){
        const char *path=spec+5;
        fd=open(path,O_RDWR);
        if(fd<0) die_perror("open(file)");
        rb=mmap(NULL,sizeof(ISYRing),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        if(rb==MAP_FAILED) die_perror("mmap(file)");
    }else{
        fd=shm_open(spec,O_RDWR,0600);
        if(fd<0) die_perror("shm_open");
        rb=mmap(NULL,sizeof(ISYRing),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        if(rb==MAP_FAILED) die_perror("mmap(shm)");
    }

    sem_t *sem=sem_open(sem_name,0);
    if(sem==SEM_FAILED) die_perror("sem_open");

    /* États bannière */
    int admin_on=0,idle_on=0;
    char admin[TXT_LEN],idle[TXT_LEN];
    infer_banners_from_ring(rb,&admin_on,admin,&idle_on,idle);

    // Premier rendu
    rerender_all(admin_on,admin,idle_on,idle);

    while(1){
        sem_wait(sem);
        while(rb->ridx!=rb->widx){
            ISYMsg *m=&rb->ring[rb->ridx%SHM_RING_CAP];
            const char *t=m->Texte;

            if(starts_with(t,"CTRL BANNER_SET ")){
                strncpy(admin,t+16,TXT_LEN-1); admin[TXT_LEN-1]='\0'; admin_on=1;
                rerender_all(admin_on,admin,idle_on,idle);
            }else if(!strcmp(t,"CTRL BANNER_CLR")){
                admin_on=0; admin[0]='\0';
                rerender_all(admin_on,admin,idle_on,idle);
            }else if(starts_with(t,"CTRL IBANNER_SET ")){
                strncpy(idle,t+18,TXT_LEN-1); idle[TXT_LEN-1]='\0'; idle_on=1;
                rerender_all(admin_on,admin,idle_on,idle);
            }else if(!strcmp(t,"CTRL IBANNER_CLR")){
                idle_on=0; idle[0]='\0';
                rerender_all(admin_on,admin,idle_on,idle);
            }else{
                push_line(t);
                rerender_all(admin_on,admin,idle_on,idle);
            }

            rb->ridx++;
        }
        if(rb->closed && rb->ridx==rb->widx) break;
    }

    munmap(rb,sizeof *rb);
    if(fd>=0) close(fd);
    sem_close(sem);
    return 0;
}
