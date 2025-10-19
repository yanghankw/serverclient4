// server.c
// Compile: gcc server.c -o server -lpthread
// Run: ./server <port>
// Example: ./server 12345

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define MAX_CLIENTS 5
#define BUF_SIZE 1024

typedef struct {
    int sockfd;
    int id;             // client id (1..)
    char room;          // 'A','B','C' or 0
    int active;         // 1 active, 0 free
} client_t;

client_t clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
int disconnected_count = 0;

// ---------- Utility functions (special features as functions) ----------
void send_to_client(int sockfd, const char *msg) {
    if (sockfd <= 0) return;
    send(sockfd, msg, strlen(msg), 0);
}

int add_client(int sockfd) {
    pthread_mutex_lock(&clients_mutex);
    int i;
    for (i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].active) {
            clients[i].active = 1;
            clients[i].sockfd = sockfd;
            clients[i].id = i + 1; // id start from 1
            clients[i].room = 0; // not yet in room
            pthread_mutex_unlock(&clients_mutex);
            return clients[i].id;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return -1; // full
}

void remove_client_by_index(int idx) {
    pthread_mutex_lock(&clients_mutex);
    if (clients[idx].active) {
        close(clients[idx].sockfd);
        clients[idx].active = 0;
        clients[idx].sockfd = 0;
        clients[idx].room = 0;
        // count disconnects
        disconnected_count++;
        printf("[Server] Client %d disconnected. Total disconnected: %d\n", idx+1, disconnected_count);
    }
    pthread_mutex_unlock(&clients_mutex);
}

void remove_client_by_id(int id) {
    if (id <=0) return;
    int idx = id - 1;
    remove_client_by_index(idx);
}

// Broadcast message to all clients in same room (except optional exclude_sockfd)
void broadcast_room(char room, const char *msg, int exclude_sockfd) {
    pthread_mutex_lock(&clients_mutex);
    for (int i=0;i<MAX_CLIENTS;++i) {
        if (clients[i].active && clients[i].room == room) {
            if (clients[i].sockfd != exclude_sockfd) {
                send_to_client(clients[i].sockfd, msg);
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Broadcast message to ALL clients (server-wide)
void broadcast_all(const char *msg) {
    pthread_mutex_lock(&clients_mutex);
    for (int i=0;i<MAX_CLIENTS;++i) {
        if (clients[i].active) {
            send_to_client(clients[i].sockfd, msg);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Change room for a client id
void change_room(int client_id, char new_room) {
    if (client_id<=0) return;
    pthread_mutex_lock(&clients_mutex);
    int idx = client_id - 1;
    if (clients[idx].active) {
        clients[idx].room = new_room;
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Find index by sockfd (or -1)
int find_index_by_sock(int sockfd) {
    int idx = -1;
    pthread_mutex_lock(&clients_mutex);
    for (int i=0;i<MAX_CLIENTS;++i) if (clients[i].active && clients[i].sockfd==sockfd) { idx=i; break; }
    pthread_mutex_unlock(&clients_mutex);
    return idx;
}

// ---------- Client handling thread ----------
void *handle_client(void *arg) {
    int sockfd = *((int*)arg);
    free(arg);

    // add client (should have been added earlier), find id:
    int idx = find_index_by_sock(sockfd);
    if (idx == -1) {
        close(sockfd);
        return NULL;
    }
    int client_id = idx + 1;

    char buf[BUF_SIZE];
    memset(buf,0,sizeof(buf));
    // ask client to choose room
    send_to_client(sockfd, "Welcome! Enter room: A / B / C (example: A)\n");

    while (1) {
        int n = recv(sockfd, buf, sizeof(buf)-1, 0);
        if (n <= 0) {
            // disconnect
            printf("[Server] recv <=0 from client %d, closing.\n", client_id);
            remove_client_by_id(client_id);
            break;
        }
        buf[n]=0;
        // strip trailing \r\n
        while (n>0 && (buf[n-1]=='\n' || buf[n-1]=='\r')) { buf[n-1]=0; --n; }

        if (n==0) continue;

        // check commands:
        if (strcmp(buf, "EXIT!") == 0) {
            send_to_client(sockfd, "Goodbye!\n");
            remove_client_by_id(client_id);
            break;
        }
        // change room command (prefix "/room " or single letter A/B/C)
        if (buf[0]=='/' && strncmp(buf, "/room ", 6)==0) {
            char r = buf[6];
            if (r=='A'||r=='B'||r=='C') {
                char old_room;
                pthread_mutex_lock(&clients_mutex);
                old_room = clients[idx].room;
                clients[idx].room = r;
                pthread_mutex_unlock(&clients_mutex);

                char notify[256];
                snprintf(notify, sizeof(notify), "[Server] Client %d moved to Room%c.\n", client_id, r);
                send_to_client(sockfd, notify);

                // inform others in new room
                snprintf(notify, sizeof(notify), "[Server] Client %d joined Room%c.\n", client_id, r);
                broadcast_room(r, notify, sockfd);
            } else {
                send_to_client(sockfd, "Invalid room. Use A or B or C\n");
            }
            continue;
        }
        // if single letter A/B/C (join first time or change)
        if ((strlen(buf)==1) && (buf[0]=='A'||buf[0]=='B'||buf[0]=='C')) {
            char r = buf[0];
            pthread_mutex_lock(&clients_mutex);
            clients[idx].room = r;
            pthread_mutex_unlock(&clients_mutex);

            // notify the joining client
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "You joined Room%c\n", r);
            send_to_client(sockfd, tmp);

            // notify others in same room
            snprintf(tmp, sizeof(tmp), "[Server] Client %d joined Room%c.\n", client_id, r);
            broadcast_room(r, tmp, sockfd);
            continue;
        }

        // server broadcast from server console will be prefixed "/announce " and handled by server main thread
        // otherwise normal chat message -> broadcast to same room
        pthread_mutex_lock(&clients_mutex);
        char current_room = clients[idx].room;
        pthread_mutex_unlock(&clients_mutex);

        if (current_room == 0) {
            send_to_client(sockfd, "You are not in any room. Enter A/B/C or use /room <A|B|C>\n");
            continue;
        }

        // compose message: Client# : message
        char msg[1200];
        snprintf(msg, sizeof(msg), "Client%d@Room%c: %s\n", client_id, current_room, buf);
        broadcast_room(current_room, msg, sockfd);
    }

    return NULL;
}

// server console thread to allow server admin to broadcast announcements
void *server_console(void *arg) {
    (void)arg;
    char line[BUF_SIZE];
    while (fgets(line, sizeof(line), stdin)) {
        // strip newline
        int L = strlen(line);
        while (L>0 && (line[L-1]=='\n'||line[L-1]=='\r')) { line[L-1]=0; --L; }
        if (L==0) continue;

        if (strncmp(line, "/announce ", 10) == 0) {
            char announce[1200];
            snprintf(announce, sizeof(announce), "[ANNOUNCE] %s\n", line+10);
            broadcast_all(announce);
            printf("[Server] broadcasted announcement.\n");
        } else if (strcmp(line, "/list")==0) {
            pthread_mutex_lock(&clients_mutex);
            printf("Client list:\n");
            for (int i=0;i<MAX_CLIENTS;++i) {
                if (clients[i].active)
                    printf("  id=%d sock=%d room=%c\n", clients[i].id, clients[i].sockfd, clients[i].room ? clients[i].room : '-');
                else
                    printf("  slot %d empty\n", i+1);
            }
            pthread_mutex_unlock(&clients_mutex);
        } else {
            printf("Use /announce <msg> to broadcast to all clients, /list to view clients\n");
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    int port = atoi(argv[1]);

    // init clients
    for (int i=0;i<MAX_CLIENTS;++i) {
        clients[i].active = 0;
        clients[i].sockfd = 0;
        clients[i].id = i+1;
        clients[i].room = 0;
    }

    signal(SIGPIPE, SIG_IGN); // ignore broken pipe

    int listenfd;
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in servaddr;
    memset(&servaddr,0,sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(port);

    if (bind(listenfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(listenfd, 10) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    printf("[Server] Listening on port %d\n", port);

    // start server console thread
    pthread_t console_tid;
    pthread_create(&console_tid, NULL, server_console, NULL);
    pthread_detach(console_tid);

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int connfd = accept(listenfd, (struct sockaddr*)&cliaddr, &clilen);
        if (connfd < 0) {
            perror("accept");
            continue;
        }

        // Add client; check full
        int id = add_client(connfd);
        if (id == -1) {
            // server full. send message and close
            const char *msg = "Server is full!\n";
            send_to_client(connfd, msg);
            close(connfd);
            printf("[Server] Rejected incoming connection: server full.\n");
            continue;
        }

        printf("[Server] New connection accepted, assigned id=%d sock=%d\n", id, connfd);

        // send welcome message
        char welcome[256];
        snprintf(welcome, sizeof(welcome), "Welcome! Your client id is %d\n", id);
        send_to_client(connfd, welcome);

        // create thread to handle client
        pthread_t tid;
        int *pclient = malloc(sizeof(int));
        *pclient = connfd;
        if (pthread_create(&tid, NULL, handle_client, pclient) != 0) {
            perror("pthread_create");
            remove_client_by_id(id);
            free(pclient);
        } else {
            pthread_detach(tid);
        }
    }

    close(listenfd);
    return 0;
}
