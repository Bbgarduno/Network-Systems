#include "server.h"

int main(int argc, char **argv){
    int clientfd, portno;   // Socket, port to listen, bytesize of client address
    socklen_t clientlen;
    struct sockaddr_in serveraddr;  // server's addr
    struct sockaddr_in clientaddr;  // client's addr
    struct hostent *hostp;          // client host info
    char buf[BUFSIZE];              // message buffer
    int previous;                   // prev recved msg
    char command[INPUTSIZE];        // recved cmd
    char filename[INPUTSIZE];       // recved filename
    char *hostaddrp;                // dotted decimal host addr string
    int optval;                     // flag value for setsockopt
    int n;                          // message byte size
    FILE *fptr;

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
    if (sockfd == 0){
        error("Error opening socket");
    }
    
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *) &optval, sizeof(int));
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short) portno);

    if (bind(sockfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0)
        error("Error on binding");

    clientlen = sizeof(clientaddr);

    /*
    Listen on a continuous loop for client connections
    When a connection is establish and accepted spawn a new thread so resources are still available for new incoming clients
    In each thread, handle requests
    */

    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, sigint_handler);

    while (1) {
        bzero(buf, BUFSIZE);
        /* Get client information */
        if (listen(sockfd, 3) < 0)
            error("ERROR in recvfrom");
        if ((clientfd = accept(sockfd, (struct sockaddr *)&clientaddr, (socklen_t *)&clientlen)) < 0)
            error("ERROR in accept");
        if ((hostaddrp = inet_ntoa(clientaddr.sin_addr)) == NULL)
            error("ERROR on inet_ntoa\n");

        printf("server received request from %s\n", hostaddrp);

        while(processes > 8){
            printf("Max processes reached\n");
            sleep(1);
        }

        pid_t tid = fork();
        if (tid < 0)
            perror("ERROR could not fork");
        if (tid == 0){
            close(sockfd);
            handleClient(clientfd);
            exit(0);
        } else {
            close(clientfd);
            processes++;
        }
    }
    
    return 0;
}