#include "server.h"

int main(int argc, char **argv){
    int sockfd, portno;   // Socket, port to listen, bytesize of client address
    int *clientfd;
    socklen_t clientlen;
    struct sockaddr_in serveraddr;  // server's addr
    struct sockaddr_in clientaddr;  // client's addr
    struct hostent *hostp;          // client host info
    char *hostaddrp;                // dotted decimal host addr string
    int optval;                     // flag value for setsockopt
    pthread_t tid;

    if (argc != 2){
        fprintf(stderr, "%s [port]\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);
    if (!portno || portno <= 1024 || portno >= 65535){
        fprintf(stderr, "invalid port number [1025-65524]\n");
        exit(0);

    }
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0){
        perror("ERROR opening socket");
        exit(1);
    }
    
    optval = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *) &optval, sizeof(int)) < 0){
        perror("ERROR with setsockopt");
        exit(1);
    }
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short) portno);

    if (bind(sockfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0){
        perror("Error on binding");
        exit(1);
    }

    clientlen = sizeof(clientaddr);

    /*
    Listen on a continuous loop for client connections
    When a connection is establish and accepted spawn a new thread so resources are still available for new incoming clients
    In each thread, handle requests
    */

    if (signal(SIGINT, sigint_handler) == SIG_ERR){
        perror("ERROR in signal");
        close(sockfd);
        exit(1);
    }
    
    if (listen(sockfd, 50) < 0){
        perror("ERROR in listen");
        close(sockfd);
        exit(1);
    }
    
    while (1) {
        /* Get client information */
        clientfd = malloc(sizeof(int));
        if ((*clientfd = accept(sockfd, (struct sockaddr *)&clientaddr, (socklen_t *)&clientlen)) < 0){
            perror("ERROR in accept");
            free(clientfd);
            exit(1);
        }
        if ((hostaddrp = inet_ntoa(clientaddr.sin_addr)) == NULL){
            perror("ERROR on inet_ntoa\n");
            free(clientfd);
            exit(1);
        }

        printf("server received request from %s\n", hostaddrp);

        if (pthread_create(&tid, NULL, handleClient, clientfd) != 0){
            perror("ERROR creating thread");
            free(clientfd);
            exit(1);
        }
        pthread_detach(tid);
    }
}