// client.c
// Compile: gcc client.c -o client -lpthread
// Run: ./client <server_ip> <port>
// Example: ./client 127.0.0.1 12345

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>

#define BUF_SIZE 1024

int sockfd;
volatile int running = 1;

void *client_recv(void *arg) {
    (void)arg;
    char buf[BUF_SIZE];
    while (running) {
        int n = recv(sockfd, buf, sizeof(buf)-1, 0);
        if (n <= 0) {
            printf("[Client] Connection closed by server.\n");
            running = 0;
            break;
        }
        buf[n]=0;
        printf("%s", buf);
        fflush(stdout);
    }
    return NULL;
}

void *client_send(void *arg) {
    (void)arg;
    char line[BUF_SIZE];
    while (running && fgets(line, sizeof(line), stdin)) {
        // strip \n
        int L = strlen(line);
        while (L>0 && (line[L-1]=='\n' || line[L-1]=='\r')) { line[L-1]=0; --L; }
        if (L==0) continue;

        // if server full message from server will be printed by recv thread and server will close connection
        if (strcmp(line, "EXIT!") == 0) {
            send(sockfd, line, strlen(line), 0);
            usleep(100*1000); // small delay to allow server reply
            running = 0;
            break;
        }

        // send input directly
        send(sockfd, line, strlen(line), 0);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <server_ip> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    char *server_ip = argv[1];
    int port = atoi(argv[2]);

    struct sockaddr_in servaddr;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    memset(&servaddr,0,sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &servaddr.sin_addr) <= 0) {
        perror("inet_pton");
        exit(EXIT_FAILURE);
    }

    if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect");
        exit(EXIT_FAILURE);
    }

    pthread_t rt, st;
    pthread_create(&rt, NULL, client_recv, NULL);
    pthread_create(&st, NULL, client_send, NULL);

    pthread_join(st, NULL);
    // if send thread ends, close socket to cause recv thread to exit
    close(sockfd);
    running = 0;
    pthread_join(rt, NULL);

    printf("[Client] Exited.\n");
    return 0;
}
