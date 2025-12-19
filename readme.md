# ISY Chat — Serveur / Groupes / Clients (UDP)

Projet de messagerie en groupes, composé de 4 programmes :
- **ServeurISY** : gère la création, la liste, la jonction et la fusion des groupes.
- **GroupeISY** : gère une discussion (un groupe), ses membres, bannières, modération, inactivité.
- **ClientISY** : gère les commandes côté client + réseau (UDP) + logique (admin/merge/ban).
- **AffichageISY** : gère l’affichage terminal (bannières “pinnées”, historique, prompt) et remonte les entrées clavier au client.

Le tout s’appuie sur **Commun.h** (constantes, tailles, utilitaires communs).

---

## Sommaire
- [Fonctionnalités](#fonctionnalités)
- [Architecture](#architecture)
- [Compilation](#compilation)
- [Configuration](#configuration)
- [Lancement](#lancement)
- [Commandes client](#commandes-client)
- [Commandes serveur (admin console)](#commandes-serveur-admin-console)
- [Fusion de groupes](#fusion-de-groupes)
- [Modération (ban/unban)](#modération-banunban)
- [Inactivité et suppression de groupe](#inactivité-et-suppression-de-groupe)
- [Détails réseau (UDP)](#détails-réseau-udp)
- [Dépannage](#dépannage)
- [Structure du dépôt](#structure-du-dépôt)

---

## Fonctionnalités

### Basique
- Création de groupe
- Liste des groupes existants
- Rejoindre un groupe
- Envoyer / recevoir des messages dans un groupe

### Affichage (AffichageISY)
- Bannières (serveur / inactivité) **toujours visibles en haut** (pinnées)
- Wrap automatique des bannières si le terminal est petit
- Historique limité et affichage “fenêtré” (le bas de l’historique reste visible)

### Admin / Gestion
- **Token admin** attribué à la création (si CREATE inclut l’utilisateur)
- **Modération** : bannir / débannir un membre (ban persistant dans le groupe)
- **Fusion** : redirection automatique des clients du groupe B vers le groupe A
- **Annonce d’actions** : messages `[Action] (...)` dans le chat (ban/unban/fusion)

### Robustesse
- Gestion du Ctrl-C (serveur et client) sans blocage
- Tolérance aux pertes UDP : on évite les resets d’état sur absence de réponse ponctuelle

---

## Architecture

### Processus
- **ClientISY** lance automatiquement **AffichageISY** via `fork/exec`.
- Communication ClientISY ↔ AffichageISY via deux FIFOs :
  - `/tmp/isy_ui_in_<pid>`  (Client → UI)
  - `/tmp/isy_ui_out_<pid>` (UI → Client)

### Réseau (UDP)
- **ServeurISY** écoute sur `SERVER_IP:SERVER_PORT` (ex: `0.0.0.0:8000`)
- Chaque **GroupeISY** écoute sur un port distinct `BASE_PORT + index`
- Le **ClientISY** :
  - envoie les commandes serveur via `sock_srv` vers `SERVER_IP:SERVER_PORT`
  - envoie les messages au groupe vers `SERVER_IP:<port_du_groupe>`
  - reçoit les messages sur `LOCAL_RECV_PORT` (bind local)

---

## Compilation

### Prérequis
- `cc` / `clang` / `gcc`
- `make`
- pthreads

### Build
```bash
make
Clean
bash
Copier le code
make clean
Le Makefile détecte macOS vs Linux/WSL et ajuste les flags/Libs.

Configuration
conf/server.conf
Exemple :

ini
Copier le code
SERVER_IP=0.0.0.0
SERVER_PORT=8000
BASE_PORT=8010
MAX_GROUPS=32

# Timeout d'inactivité (en secondes) injecté dans GroupeISY
IDLE_TIMEOUT_SEC=1800
conf/client.conf
Exemple :

ini
Copier le code
USER=franck
SERVER_IP=127.0.0.1
SERVER_PORT=8000

# Port local sur lequel le client reçoit les messages du groupe
LOCAL_RECV_PORT=9001
Lancement
1) Démarrer le serveur
bash
Copier le code
./ServeurISY conf/server.conf
2) Lancer un client
bash
Copier le code
./ClientISY conf/client.conf
Le client ouvre automatiquement l’interface terminal (AffichageISY).
Utilise un port LOCAL_RECV_PORT différent pour chaque client.

Commandes client
Menu principal
Dans l’UI, choisir :

0 : Créer un groupe

1 : Rejoindre un groupe

2 : Lister les groupes

3 : Dialoguer sur un groupe

4 : Quitter

5 : Quitter le groupe

Mode “dialogue”
Quand tu es dans un groupe et que tu dialogues :

cmd : passer en mode commandes

msg : revenir en mode message

quit : revenir au menu principal

Important : les messages entrants du groupe ne sont affichés que si tu es en mode “dialogue”.

Mode cmd (admin / gestion)
help : affiche l’aide

admin : liste les tokens enregistrés

settoken <groupe> <token> : enregistrer un token manuellement

ban <pseudo> : bannir un membre du groupe courant

unban <pseudo> : débannir un membre du groupe courant

merge <A> <B> : fusionner B vers A (il faut être admin des deux)

Commandes serveur (admin console)
Dans la console ServeurISY :

/banner <texte> : définit une bannière serveur (tous groupes)

/banner_clr : efface la bannière serveur

/sys <texte> : envoie un message SYS (tous groupes)

/list : liste les groupes actifs (port, pid, token)

/quit : stop serveur (Ctrl-C fonctionne aussi)

Fusion de groupes
But : fusionner B → A (les clients de B basculent vers A).

Étapes
Créer deux groupes via le même client ou deux clients :

Le créateur reçoit un token admin pour chaque groupe (stocké dans ClientISY).

Entrer en dialogue, puis :

cmd

merge GRP_A GRP_B

Résultat
Serveur envoie CTRL REDIRECT au groupe B

Groupe B broadcast CTRL REDIRECT ... à ses clients

Les clients de B :

quittent B

changent de port vers A

envoient (joined) pour récupérer bannières actives

Modération (ban/unban)
Ban
Dans un groupe (mode cmd) :

text
Copier le code
ban Pseudo
Effets :

le groupe enregistre le pseudo banni (persistant dans le process du groupe)

supprime le membre des membres actifs

broadcast une ligne [Action] (admin) a banni (...) ou avec le nom admin si fourni

Unban
text
Copier le code
unban Pseudo
Effets :

retire le pseudo de la liste de bans

broadcast [Action] ... a debanni ...

Inactivité et suppression de groupe
Chaque GroupeISY surveille son activité :

si inactivité dépasse un seuil (ex: IDLE_TIMEOUT_SEC)

affiche une bannière d’avertissement avant suppression

à expiration :

broadcast un message SYS indiquant que le groupe est supprimé

le groupe se termine

Côté client :

le client détecte le message de suppression

conseille quit pour revenir au menu

au prochain passage menu, l’état peut être reset proprement

Détails réseau (UDP)
Le projet utilise UDP (non fiable par nature : pertes possibles).

Pour limiter les impacts :

server_list_and_find() tente plusieurs fois

on distingue “pas de réponse” (réseau) de “LIST reçu mais groupe absent” (groupe réellement supprimé)

Exécution sur Internet
Mettre SERVER_IP=0.0.0.0 côté serveur (bind)

Ouvrir les ports nécessaires (firewall/NAT) :

SERVER_PORT (ex: 8000)

plage des ports groupes : BASE_PORT → BASE_PORT + MAX_GROUPS - 1

Côté clients, SERVER_IP doit être l’IP publique ou DNS du serveur.

Dépannage
“Je reçois les messages quand je suis au menu”
Normalement non : le client n’affiche les messages entrants que si in_dialogue=1.
Si ça arrive, vérifier que tu es bien sur la dernière version des fichiers.

“Merge ne fait rien”
Vérifier que tu as bien des tokens admin pour A et B (cmd → admin)

Vérifier que le serveur est bien lancé et répond

Vérifier que les deux groupes existent réellement (2 → LIST)

Vérifier que les ports groupe sont ouverts si exécution via Internet

“Le client se fait éjecter du groupe en entrant en dialogue”
Cause classique : LIST perdu (UDP). La logique actuelle évite le reset si LIST ne répond pas.
Si tu es encore concerné, vérifier que server_list_and_find() retourne bien -1 sur absence de réponse et que le code ne fait pas de cleanup dans ce cas.

“Ctrl-C ne fonctionne pas”
Serveur : SO_RCVTIMEO est utilisé pour réveiller la boucle.

Client : le handler stoppe l’UI et envoie un (left) si possible.

Structure du dépôt
css
Copier le code
.
├── src/
│   ├── Commun.h
│   ├── ServeurISY.c
│   ├── GroupeISY.c
│   ├── ClientISY.c
│   └── AffichageISY.c
├── conf/
│   ├── server.conf
│   └── client.conf
├── Makefile
└── README.md
Notes
Ce projet est conçu pour un usage pédagogique.

Les bans sont persistants dans la vie du process du groupe (si GroupeISY redémarre, la liste est perdue).

Le transport UDP n’assure pas la livraison : une implémentation “production” utiliserait TCP ou un protocole fiable au-dessus d’UDP.