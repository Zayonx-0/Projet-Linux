#include "Commun.h"
#include <sys/stat.h>
#include <limits.h>

static void usage(const char *p){ fprintf(stderr, "Usage: %s <shm_or_file_spec> <sem_name>\n", p); }

static int starts_with(const char *s, const char *p){ return strncmp(s,p,strlen(p))==0; }

int main(int argc, char **argv){
    if(argc<3){ usage(argv[0]); return 1; }
    const char *spec = argv[1];         // soit "/nom_shm", soit "file:/tmp/isy_user_group.ring"
    const char *sem_name = argv[2];

    ISYRing *rb = NULL;
    int fd = -1;
    int use_file = 0;

    if (starts_with(spec, "file:")) {
        const char *path = spec + 5;
        use_file = 1;
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

    printf("AffichageISY: attaché à %s\n", spec);

    while(1){
        sem_wait(sem);
        while (rb->ridx != rb->widx){
            ISYMsg *m = &rb->ring[rb->ridx % SHM_RING_CAP];
            printf("%.*s: %.*s\n", EME_LEN, m->Emetteur, TXT_LEN, m->Texte);
            rb->ridx++;
        }
        if(rb->closed && rb->ridx==rb->widx) break;
        fflush(stdout);
    }

    munmap(rb, sizeof *rb);
    if (use_file) close(fd); else { close(fd); }
    sem_close(sem);
    return 0;
}
