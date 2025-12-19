// src/GroupeISY.c
#include "Commun.h"
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/*
    ─────────────────────────────────────────────────────────────────────────
    GroupeISY
    ─────────────────────────────────────────────────────────────────────────
    Rôle :
      - Un processus GroupeISY gère UN SEUL groupe de discussion.
      - Il reçoit des datagrammes UDP de deux sources :
          1) Les clients :     "MSG ..." ou "CMD ..."
          2) Le serveur :      "CTRL ..." ou "SYS ..." (canal admin local)
      - Il maintient en mémoire :
          - la liste des membres connectés (pseudo + adresse UDP)
          - une liste de pseudos bannis (ban persistant tant que le groupe vit)
          - l’état de 2 bannières :
              * bannière admin (fixée par ServeurISY via CTRL BANNER_SET/CLR)
              * bannière inactivité (gérée par un timer interne)
      - Il diffuse les messages à tous les membres (broadcast).
      - Il supprime le groupe automatiquement après un temps d’inactivité, après
        avoir averti via une bannière dédiée.

    Remarque :
      - Le protocole étant UDP, il n’y a pas de "connexion" TCP : on associe un pseudo
        à l’adresse UDP de son client quand on reçoit des MSG/CMD.
      - Pour que le groupe puisse "ré-afficher" les bannières à un client qui rejoint,
        le client envoie un message "MSG <user> (joined)" : cela sert de handshake.
*/

#define MAX_MEMBERS 64  // nombre max d’utilisateurs simultanés dans le groupe
#define MAX_BANS    128 // liste max de pseudos bannis (en mémoire)

/*
    Un membre du groupe :
      - user  : pseudo
      - addr  : adresse UDP (IP:port) du client, pour répondre/broadcaster
      - inuse : slot utilisé ou non
*/
typedef struct {
    char user[EME_LEN];
    struct sockaddr_in addr;
    int inuse;
} Member;

/*
    Entrée de ban :
      - user  : pseudo banni
      - inuse : slot utilisé ou non
*/
typedef struct {
    char user[EME_LEN];
    int inuse;
} BanRec;

/* Flag global d’exécution : modifié par SIGINT/SIGTERM et par le timer inactivité */
static volatile sig_atomic_t running = 1;

/* Handler de signal : stoppe la boucle principale */
static void on_sigint(int signo){
    (void)signo;
    running = 0;
}

/*
    Envoi UDP utilitaire :
      - txt : chaîne déjà formée ("CTRL ...", "SYS ...", "GROUPE[...] ...")
      - to  : destination (sockaddr_in)
*/
static inline void send_txt(int s, const char *txt, const struct sockaddr_in *to){
    (void)sendto(s, txt, strlen(txt), 0, (const struct sockaddr*)to, sizeof *to);
}

/*
    Définit un timeout de réception (SO_RCVTIMEO).
    But :
      - éviter de rester bloqué sur recvfrom()
      - permettre à la boucle de vérifier régulièrement running
*/
static void set_rcv_timeout(int sock, int ms){
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

/* ───────────────────────── Etat du groupe ───────────────────────── */

// Membres connectés + liste des bannis
static Member members[MAX_MEMBERS];
static BanRec bans[MAX_BANS];

// Mutex global : protège members[], bans[], last_activity, bannières, token, etc.
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

// Bannière "admin" (fixée par le serveur / commandes CTRL)
static int  admin_banner_active = 0;
static char admin_banner[TXT_LEN];

// Bannière "inactivité" (gérée par le timer)
static int  idle_banner_active = 0;
static char idle_banner[TXT_LEN];

// Token admin local du groupe (contrôle de modération)
static char g_admin_token[ADMIN_TOKEN_LEN] = {0};

// Gestion de l’inactivité
static unsigned idle_timeout_sec = 1800; // configuré par argv[3], sinon défaut
static time_t   last_activity = 0;       // dernière activité (MSG ou CMD reçu)

// Pour afficher le nom du groupe dans les broadcasts
static char     gname_local[32] = {0};
static uint16_t gport_local = 0;

/* ───────────────────────── Ban helpers ───────────────────────── */
/*
    Les fonctions suffixées _nolock supposent que mtx est déjà acquis.
    Cela évite de verrouiller/déverrouiller plusieurs fois pour une même action.
*/

/* Retourne 1 si user est banni, 0 sinon */
static int ban_is_banned_nolock(const char *user){
    for(int i=0;i<MAX_BANS;i++){
        if(bans[i].inuse && !strcmp(bans[i].user, user)) return 1;
    }
    return 0;
}

/* Ajoute un pseudo à la liste des bannis. Retourne 1 si OK, 0 si liste pleine. */
static int ban_add_nolock(const char *user){
    if(ban_is_banned_nolock(user)) return 1;

    for(int i=0;i<MAX_BANS;i++){
        if(!bans[i].inuse){
            bans[i].inuse = 1;
            isy_strcpy(bans[i].user, sizeof bans[i].user, user);
            return 1;
        }
    }
    return 0;
}

/* Retire un pseudo de la liste des bannis. Retourne 1 si supprimé, 0 sinon. */
static int ban_remove_nolock(const char *user){
    for(int i=0;i<MAX_BANS;i++){
        if(bans[i].inuse && !strcmp(bans[i].user, user)){
            bans[i].inuse = 0;
            bans[i].user[0] = '\0';
            return 1;
        }
    }
    return 0;
}

/* ───────────────────────── Member helpers ───────────────────────── */

/* Recherche un membre dans members[]. Retourne index ou -1 si absent. */
static int member_find_nolock(const char *user){
    for(int i=0;i<MAX_MEMBERS;i++){
        if(members[i].inuse && !strcmp(members[i].user, user)) return i;
    }
    return -1;
}

/*
    Ajoute un membre si absent, ou met à jour son adresse si déjà présent.
    Retour :
      - index du membre si OK
      - -1 si le groupe est plein
*/
static int member_add_or_update_nolock(const char *user, const struct sockaddr_in *addr){
    int idx = member_find_nolock(user);
    if(idx < 0){
        for(int i=0;i<MAX_MEMBERS;i++){
            if(!members[i].inuse){
                members[i].inuse = 1;
                isy_strcpy(members[i].user, sizeof members[i].user, user);
                members[i].addr = *addr;
                return i;
            }
        }
        return -1;
    }

    // Membre déjà présent : on met à jour l’adresse (utile si le client change de port)
    members[idx].addr = *addr;
    return idx;
}

/* Supprime un membre (ex: (left) ou ban) */
static void member_remove_nolock(const char *user){
    int idx = member_find_nolock(user);
    if(idx >= 0){
        members[idx].inuse = 0;
        members[idx].user[0] = '\0';
        memset(&members[idx].addr, 0, sizeof members[idx].addr);
    }
}

/* ───────────────────────── Broadcast helpers ───────────────────────── */

/* Diffuse un payload brut à tous les membres */
static void broadcast_to_all_nolock(int s, const char *payload){
    for(int i=0;i<MAX_MEMBERS;i++){
        if(members[i].inuse){
            send_txt(s, payload, &members[i].addr);
        }
    }
}

/*
    Diffuse une "ligne de chat" normalisée.
    On préfixe par GROUPE[<nom>] pour que le client sache clairement de quel groupe vient le message.
*/
static void broadcast_group_line_nolock(int s, const char *line){
    char out[TXT_LEN + 128];
    snprintf(out, sizeof out, "GROUPE[%s]: %s", gname_local, line);
    broadcast_to_all_nolock(s, out);
}

/* ───────────────────────── Admin token logic ───────────────────────── */
/*
    Vérifie / initialise le token admin.
    - Si g_admin_token est vide, la première commande admin reçue peut l'initialiser (fallback).
    - Sinon, on exige l’égalité exacte.

    But :
      - Permet d'utiliser le token fourni par ServeurISY
      - Mais reste robuste même si le token n’a pas été "SETTOKEN" au démarrage.
*/
static int ensure_or_check_admin_token_locked(const char *tok){
    if(!tok || !*tok) return 0;

    // Fallback : 1ère commande admin configure le token si inconnu
    if(g_admin_token[0] == '\0'){
        isy_strcpy(g_admin_token, sizeof g_admin_token, tok);
        return 1;
    }

    return (strcmp(g_admin_token, tok) == 0);
}

/* ───────────────────────── Timer Inactivité ───────────────────────── */
/*
    Thread dédié :
      - surveille last_activity
      - si le groupe devient inactif :
          * affiche une bannière "inactivité"
      - si aucune activité jusqu’au timeout complet :
          * envoie un message SYS
          * stoppe le groupe (running=0)
*/
typedef struct {
    int sock;
} TimerCtx;

/* Formate une heure locale HH:MM:SS */
static void fmt_hhmmss(time_t t, char *out, size_t n){
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, n, "%H:%M:%S", &tmv);
}

/*
    Thread timer :
      - Warn threshold : idle_timeout/2 (ou idle_timeout si trop petit)
      - Bannière envoyée 1 seule fois (idle_banner_active sert de garde-fou)
*/
static void *idle_timer_thread(void *arg){
    TimerCtx *ctx = (TimerCtx*)arg;

    for(;;){
        if(!running) break;
        sleep(1);

        // Désactive le mécanisme si timeout = 0
        if(idle_timeout_sec == 0) continue;

        time_t now = time(NULL);
        time_t since;

        int need_warn = 0;
        int need_clear = 0;
        int do_exit = 0;

        char warn_payload[TXT_LEN + 32];

        pthread_mutex_lock(&mtx);

        since = now - last_activity;

        // On avertit à mi-parcours (ou au max si idle_timeout est très petit)
        unsigned warn_threshold = (idle_timeout_sec >= 2 ? idle_timeout_sec / 2 : idle_timeout_sec);

        if(since >= (time_t)idle_timeout_sec){
            // Timeout atteint -> suppression du groupe
            if(idle_banner_active) need_clear = 1;
            do_exit = 1;

        } else if(since >= (time_t)warn_threshold){
            // On affiche une bannière d'avertissement (une seule fois)
            if(!idle_banner_active){
                time_t deletion_time = last_activity + (time_t)idle_timeout_sec;

                char hhmmss[16];
                fmt_hhmmss(deletion_time, hhmmss, sizeof hhmmss);

                snprintf(idle_banner, sizeof idle_banner,
                         "Inactivite detectee: le groupe '%s' sera supprime a %s sans activite.",
                         gname_local, hhmmss);

                idle_banner_active = 1;
                snprintf(warn_payload, sizeof warn_payload, "CTRL IBANNER_SET %s", idle_banner);
                need_warn = 1;
            }
        }

        pthread_mutex_unlock(&mtx);

        // Les sends se font hors lock si possible, mais ici on reprend le lock
        // pour réutiliser broadcast_to_all_nolock() (qui suppose mtx acquis).
        if(need_warn){
            pthread_mutex_lock(&mtx);
            broadcast_to_all_nolock(ctx->sock, warn_payload);
            pthread_mutex_unlock(&mtx);
        }

        if(need_clear){
            pthread_mutex_lock(&mtx);
            broadcast_to_all_nolock(ctx->sock, "CTRL IBANNER_CLR");
            pthread_mutex_unlock(&mtx);
        }

        // Suppression du groupe : message + arrêt
        if(do_exit){
            pthread_mutex_lock(&mtx);
            broadcast_to_all_nolock(ctx->sock,
                "SYS Le groupe est supprime pour cause d'inactivite. Tappez \"quit\" pour quitter.");
            pthread_mutex_unlock(&mtx);

            running = 0;
            break;
        }
    }

    return NULL;
}

/* ───────────────────────── Main ───────────────────────── */
int main(int argc, char **argv){
    if(argc < 3){
        fprintf(stderr, "Usage: %s <groupName> <port> [IDLE_TIMEOUT_SEC]\n", argv[0]);
        return 1;
    }

    // Arguments : nom groupe, port UDP du groupe, timeout optionnel
    const char *gname = argv[1];
    uint16_t gport = (uint16_t)atoi(argv[2]);
    if(argc >= 4){
        idle_timeout_sec = (unsigned)atoi(argv[3]);
    }

    // Sauvegarde locale (utile pour logs + préfix GROUPE[...])
    strncpy(gname_local, gname, sizeof gname_local - 1);
    gport_local = gport;

    // Gestion des signaux
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    // Socket UDP du groupe
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if(s < 0) die_perror("socket");

    // Réutilisation d’adresse (pratique en dev)
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    // Timeout pour permettre de quitter proprement
    set_rcv_timeout(s, 300);

    // Bind UDP sur INADDR_ANY:<port groupe>
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(gport);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if(bind(s, (struct sockaddr*)&addr, sizeof addr) < 0)
        die_perror("bind group");

    // Initialisation état
    memset(members, 0, sizeof members);
    memset(bans, 0, sizeof bans);
    admin_banner_active = 0; admin_banner[0] = '\0';
    idle_banner_active  = 0; idle_banner[0] = '\0';
    g_admin_token[0]    = '\0';
    last_activity       = time(NULL);

    fprintf(stderr, "[GroupeISY] '%s' UDP %u (idle=%us)\n",
            gname_local, (unsigned)gport_local, idle_timeout_sec);

    // Démarre le thread timer d’inactivité (détaché)
    pthread_t th_timer;
    TimerCtx tctx = {.sock = s};
    if(pthread_create(&th_timer, NULL, idle_timer_thread, &tctx) == 0){
        pthread_detach(th_timer);
    }

    // Buffer réception (messages + commandes)
    char buf[TXT_LEN + 256];

    /*
        Boucle principale :
          - reçoit un datagramme UDP
          - met à jour l’activité
          - route vers :
              CTRL / CMD / MSG / SYS
    */
    while(running){
        struct sockaddr_in cli;
        socklen_t cl = sizeof cli;

        ssize_t n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr*)&cli, &cl);
        if(n < 0){
            if(errno == EINTR) continue;
            if(errno == EAGAIN || errno == EWOULDBLOCK) continue;
            continue;
        }
        buf[n] = '\0';

        /* ───────── Activité : MSG/CMD -> reset timer + retire la bannière inactivité ───────── */
        if(!strncmp(buf, "MSG ", 4) || !strncmp(buf, "CMD ", 4)){
            pthread_mutex_lock(&mtx);

            last_activity = time(NULL);

            // Si on était en bannière d’inactivité, on la retire dès qu’il y a activité
            if(idle_banner_active){
                idle_banner_active = 0;
                broadcast_to_all_nolock(s, "CTRL IBANNER_CLR");
            }

            pthread_mutex_unlock(&mtx);
        }

        /* ───────────────────────── CTRL … (serveur -> groupe) ───────────────────────── */
        if(!strncmp(buf, "CTRL ", 5)){
            /*
                CTRL BANNER_SET <txt> :
                  - met à jour l’état local admin_banner
                  - diffuse la commande aux clients (ils l’afficheront en haut)
            */
            if(!strncmp(buf, "CTRL BANNER_SET ", 16)){
                const char *t = buf + 16;
                pthread_mutex_lock(&mtx);
                isy_strcpy(admin_banner, sizeof admin_banner, t);
                admin_banner_active = 1;
                broadcast_to_all_nolock(s, buf);
                pthread_mutex_unlock(&mtx);
                continue;
            }

            /* CTRL BANNER_CLR : retire la bannière admin */
            if(!strcmp(buf, "CTRL BANNER_CLR")){
                pthread_mutex_lock(&mtx);
                admin_banner_active = 0;
                admin_banner[0] = '\0';
                broadcast_to_all_nolock(s, buf);
                pthread_mutex_unlock(&mtx);
                continue;
            }

            /*
                CTRL IBANNER_SET / CLR :
                - utilisé pour la bannière inactivité (ou imposée si besoin)
            */
            if(!strncmp(buf, "CTRL IBANNER_SET ", 18)){
                const char *t = buf + 18;
                pthread_mutex_lock(&mtx);
                isy_strcpy(idle_banner, sizeof idle_banner, t);
                idle_banner_active = 1;
                broadcast_to_all_nolock(s, buf);
                pthread_mutex_unlock(&mtx);
                continue;
            }
            if(!strcmp(buf, "CTRL IBANNER_CLR")){
                pthread_mutex_lock(&mtx);
                idle_banner_active = 0;
                idle_banner[0] = '\0';
                broadcast_to_all_nolock(s, buf);
                pthread_mutex_unlock(&mtx);
                continue;
            }

            /*
                CTRL SETTOKEN <tok> :
                  - définit le token admin attendu pour BAN/UNBAN
            */
            if(!strncmp(buf, "CTRL SETTOKEN ", 14)){
                const char *t = buf + 14;
                pthread_mutex_lock(&mtx);
                isy_strcpy(g_admin_token, sizeof g_admin_token, t);
                pthread_mutex_unlock(&mtx);
                continue;
            }

            /*
                CTRL REDIRECT ... :
                  - cas de fusion (MERGE)
                  - on diffuse l’ordre aux clients pour qu’ils basculent automatiquement
                  - puis on arrête le groupe (après une courte pause)
            */
            if(!strncmp(buf, "CTRL REDIRECT ", 14)){
                pthread_mutex_lock(&mtx);
                broadcast_to_all_nolock(s, buf);
                pthread_mutex_unlock(&mtx);

                sleep(1);      // laisse le temps aux clients de recevoir le message
                running = 0;   // stoppe le groupe
                continue;
            }

            // Par défaut : diffuse n'importe quel CTRL inconnu
            pthread_mutex_lock(&mtx);
            broadcast_to_all_nolock(s, buf);
            pthread_mutex_unlock(&mtx);
            continue;
        }

        /* ───────────────────────── CMD … (commandes client -> groupe) ───────────────────────── */
        if(!strncmp(buf, "CMD ", 4)){
            /*
                BAN2 / UNBAN2 :
                  - format plus riche (inclut le pseudo de l'admin pour un message [Action])
                  - permet un affichage clair côté chat
            */

            // CMD BAN2 <token> <adminUser> <victim>
            if(!strncmp(buf, "CMD BAN2 ", 9)){
                char tok[ADMIN_TOKEN_LEN] = {0};
                char adminu[EME_LEN] = {0};
                char victim[EME_LEN] = {0};

                if(sscanf(buf + 9, "%63s %19s %19s", tok, adminu, victim) != 3){
                    send_txt(s, "ERR bad_args", &cli);
                    continue;
                }

                pthread_mutex_lock(&mtx);

                // Vérifie les droits admin via token
                int ok = ensure_or_check_admin_token_locked(tok);
                if(!ok){
                    pthread_mutex_unlock(&mtx);
                    send_txt(s, "ERR not_admin", &cli);
                    continue;
                }

                // Ajoute au ban + supprime des membres connectés
                ban_add_nolock(victim);
                member_remove_nolock(victim);

                // Message visible par tous pour tracer l’action
                char line[256];
                snprintf(line, sizeof line, "[Action] (%s) a banni (%s)", adminu, victim);
                broadcast_group_line_nolock(s, line);

                pthread_mutex_unlock(&mtx);

                send_txt(s, "OK banned", &cli);
                continue;
            }

            // CMD UNBAN2 <token> <adminUser> <victim>
            if(!strncmp(buf, "CMD UNBAN2 ", 11)){
                char tok[ADMIN_TOKEN_LEN] = {0};
                char adminu[EME_LEN] = {0};
                char victim[EME_LEN] = {0};

                if(sscanf(buf + 11, "%63s %19s %19s", tok, adminu, victim) != 3){
                    send_txt(s, "ERR bad_args", &cli);
                    continue;
                }

                pthread_mutex_lock(&mtx);

                int ok = ensure_or_check_admin_token_locked(tok);
                if(!ok){
                    pthread_mutex_unlock(&mtx);
                    send_txt(s, "ERR not_admin", &cli);
                    continue;
                }

                int removed = ban_remove_nolock(victim);

                if(removed){
                    char line[256];
                    snprintf(line, sizeof line, "[Action] (%s) a debanni (%s)", adminu, victim);
                    broadcast_group_line_nolock(s, line);
                    pthread_mutex_unlock(&mtx);

                    send_txt(s, "OK unbanned", &cli);
                } else {
                    pthread_mutex_unlock(&mtx);
                    send_txt(s, "OK not_banned", &cli);
                }
                continue;
            }

            /*
                Commandes legacy BAN/UNBAN (sans adminUser).
                Gardées pour compatibilité avec d’anciens clients.
            */

            // CMD BAN <token> <victim>
            if(!strncmp(buf, "CMD BAN ", 8)){
                char tok[ADMIN_TOKEN_LEN] = {0};
                char victim[EME_LEN] = {0};

                if(sscanf(buf + 8, "%63s %19s", tok, victim) != 2){
                    send_txt(s, "ERR bad_args", &cli);
                    continue;
                }

                pthread_mutex_lock(&mtx);

                int ok = ensure_or_check_admin_token_locked(tok);
                if(!ok){
                    pthread_mutex_unlock(&mtx);
                    send_txt(s, "ERR not_admin", &cli);
                    continue;
                }

                ban_add_nolock(victim);
                member_remove_nolock(victim);

                char line[256];
                snprintf(line, sizeof line, "[Action] (admin) a banni (%s)", victim);
                broadcast_group_line_nolock(s, line);

                pthread_mutex_unlock(&mtx);

                send_txt(s, "OK banned", &cli);
                continue;
            }

            // CMD UNBAN <token> <victim>
            if(!strncmp(buf, "CMD UNBAN ", 10)){
                char tok[ADMIN_TOKEN_LEN] = {0};
                char victim[EME_LEN] = {0};

                if(sscanf(buf + 10, "%63s %19s", tok, victim) != 2){
                    send_txt(s, "ERR bad_args", &cli);
                    continue;
                }

                pthread_mutex_lock(&mtx);

                int ok = ensure_or_check_admin_token_locked(tok);
                if(!ok){
                    pthread_mutex_unlock(&mtx);
                    send_txt(s, "ERR not_admin", &cli);
                    continue;
                }

                int removed = ban_remove_nolock(victim);
                if(removed){
                    char line[256];
                    snprintf(line, sizeof line, "[Action] (admin) a debanni (%s)", victim);
                    broadcast_group_line_nolock(s, line);
                    pthread_mutex_unlock(&mtx);

                    send_txt(s, "OK unbanned", &cli);
                } else {
                    pthread_mutex_unlock(&mtx);
                    send_txt(s, "OK not_banned", &cli);
                }
                continue;
            }

            // Toute autre commande inconnue
            send_txt(s, "ERR unknown_cmd", &cli);
            continue;
        }

        /* ───────────────────────── MSG <user> <text...> ───────────────────────── */
        if(!strncmp(buf, "MSG ", 4)){
            /*
                Format attendu : "MSG <user> <texte...>"
                - on extrait le pseudo
                - le reste est le texte (peut contenir des espaces)
            */
            char user[EME_LEN] = {0};
            char *p = buf + 4;

            if(sscanf(p, "%19s", user) != 1) continue;

            char *uend = strchr(p, ' ');
            if(!uend) continue;

            char *text = uend + 1;
            if(!*text) continue;

            pthread_mutex_lock(&mtx);

            // Si banni, on refuse et on ne l’ajoute pas à members[]
            if(ban_is_banned_nolock(user)){
                pthread_mutex_unlock(&mtx);
                send_txt(s, "SYS Vous etes banni de ce groupe.", &cli);
                continue;
            }

            // Ajoute/maj le membre
            int idx = member_add_or_update_nolock(user, &cli);
            if(idx < 0){
                pthread_mutex_unlock(&mtx);
                send_txt(s, "SYS Groupe plein.", &cli);
                continue;
            }

            /*
                Handshake join :
                Quand un client envoie "(joined)", on lui renvoie les bannières actives
                pour qu’elles soient affichées immédiatement après un rejoin.
            */
            if(!strcmp(text, "(joined)")){
                if(admin_banner_active){
                    char ctrl[TXT_LEN + 32];
                    snprintf(ctrl, sizeof ctrl, "CTRL BANNER_SET %s", admin_banner);
                    send_txt(s, ctrl, &members[idx].addr);
                }
                if(idle_banner_active){
                    char ctrl[TXT_LEN + 32];
                    snprintf(ctrl, sizeof ctrl, "CTRL IBANNER_SET %s", idle_banner);
                    send_txt(s, ctrl, &members[idx].addr);
                }
            }

            pthread_mutex_unlock(&mtx);

            /*
                Départ propre :
                MSG user "(left)" => on retire le membre de la table.
                Important : on le fait après avoir relâché/reloké pour garder une logique simple.
            */
            if(!strcmp(text, "(left)")){
                pthread_mutex_lock(&mtx);
                member_remove_nolock(user);
                pthread_mutex_unlock(&mtx);
            }

            // Prépare la ligne à diffuser à tous
            char line[TXT_LEN + 96];
            snprintf(line, sizeof line, "Message de %s : %s", user, text);

            pthread_mutex_lock(&mtx);
            broadcast_group_line_nolock(s, line);
            pthread_mutex_unlock(&mtx);

            continue;
        }

        /* ───────────────────────── SYS ... (serveur -> groupe -> clients) ───────────────────────── */
        if(!strncmp(buf, "SYS ", 4)){
            const char *text = buf + 4;
            if(*text){
                pthread_mutex_lock(&mtx);

                char line[TXT_LEN + 96];
                snprintf(line, sizeof line, "Message de [SERVER] : %s", text);
                broadcast_group_line_nolock(s, line);

                pthread_mutex_unlock(&mtx);
            }
            continue;
        }

        // Sinon : paquet inconnu -> ignoré (silencieux)
    }

    // Fermeture socket + log
    close(s);
    fprintf(stderr, "[GroupeISY] '%s' stopped.\n", gname_local);
    return 0;
}
