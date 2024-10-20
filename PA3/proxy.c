#include "proxy.h"

int main(int argc, char **argv){
    int portno;  // Port number
    int *clientfd; // Client socket
    socklen_t clientlen; 
    struct sockaddr_in serveraddr;  // server's addr
    struct sockaddr_in clientaddr;  // client's addr
    char *hostaddrp;                // dotted decimal host addr string
    int optval = 1;                 // flag value for setsockopt
    pthread_t tid;

    if (argc < 2 || argc > 3){
        fprintf(stderr, "%s [port] [timeout]\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);
    if (!portno || portno <= 1024 || portno >= 65535){
        fprintf(stderr, "invalid port number [1025-65524]\n");
        exit(1);
    }
    if (argc == 3){
        cacheTime = atoi(argv[2]);
        if (!cacheTime){
            fprintf(stderr, "Invalid cache timeout\n");
            exit(1);
        }
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0){
        perror("ERROR opening socket");
        exit(1);
    }
    
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
    
    if(pthread_mutex_init(&fileLock, NULL) != 0){
        perror("ERROR intializing mutex lock");
        close(sockfd);
        exit(1);
    }
    if(pthread_mutex_init(&blockLock, NULL) != 0){
        perror("ERROR intializing mutex lock");
        close(sockfd);
        exit(1);
    }
    
    if (pthread_create(&timerThread, NULL, cleanCache, NULL) != 0){
        perror("ERROR creating thread");
        close(sockfd);
        exit(1);
    }

    if (listen(sockfd, 50) < 0){
        perror("ERROR in recvfrom");
        close(sockfd);
        exit(1);
    }
    
    while (1) {
        /* Get client information */
        // struct args *arg = malloc(sizeof(struct args));
        clientfd = malloc(sizeof(int));
        if (!clientfd){
            perror("ERROR in malloc");
            close(sockfd);
            pthread_cancel(timerThread);
            exit(1);
        }

        if ((*clientfd = accept(sockfd, (struct sockaddr *)&clientaddr, (socklen_t *)&clientlen)) < 0){
            perror("ERROR in accept");
            free(clientfd);
            close(sockfd);
            pthread_cancel(timerThread);
            exit(1);
        }

        if ((hostaddrp = inet_ntoa(clientaddr.sin_addr)) == NULL){
            perror("ERROR on inet_ntoa\n");
            free(clientfd);
            close(sockfd);
            pthread_cancel(timerThread);
            exit(1);
        }

        printf("server received request from %s\n", hostaddrp);

        if (pthread_create(&tid, NULL, handleClient, clientfd) != 0){
            perror("ERROR creating thread");
            free(clientfd);
            close(sockfd);
            pthread_cancel(timerThread);
            exit(1);
        }
        pthread_detach(tid);
    }
}