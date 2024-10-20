#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <openssl/md5.h>

#define BUFSIZE 1024
#define BUFSMALL 200
#define NUMSERVER 4
int serverNum = 0;
int sockfd;
pthread_mutex_t fileLock;

void sigint_handler(int sig){
    close(sockfd);
    printf("\nServer exiting successfully\n");
    exit(EXIT_SUCCESS);
} 

void computeMD5(const char *data, char *md5sum) {
    MD5_CTX ctx;
    unsigned char digest[MD5_DIGEST_LENGTH]; // MD5 digest length is 16 bytes (128 bits)

    MD5_Init(&ctx);
    MD5_Update(&ctx, data, strlen(data));
    MD5_Final(digest, &ctx);

    // Convert the binary digest to a hex string representation
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(&md5sum[i*2], "%02x", (unsigned int)digest[i]);
    }
    md5sum[MD5_DIGEST_LENGTH * 2] = '\0'; // Null-terminate the string
}

void handleList(int clientfd){
    char buf[BUFSIZE];
    DIR *dir;
    struct dirent *entry;

    sprintf(buf, "./dfs%d", serverNum);
    dir = opendir(buf);
    if (dir == NULL) {
        perror("Error opening directory");
        strcpy(buf, "error");
        send(clientfd, buf, BUFSIZE, 0);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        // Skip "." and ".." entries
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        
        strcpy(buf, entry->d_name);
        send(clientfd, buf, BUFSIZE, 0);
    }

    send(clientfd, "", 0, 0);
    closedir(dir);
}

void handleGet(int clientfd, char *msg){
    char buf[BUFSIZE];
    char file[100];
    int index;
    FILE *fptr;
    
    sscanf(msg, "get %s %d", file, &index);
    printf("Handling message: %s\n", msg);
    
    sprintf(buf, "./dfs%d/%s/%d", serverNum, file, index);
    printf("Attempting to access file: %s\n", buf);
    pthread_mutex_lock(&fileLock);
    fptr = fopen(buf, "rb");
    bzero(buf, BUFSIZE);
    if (!fptr){
        strcpy(buf, "error");
        printf("Could not open file\n");
        send(clientfd, buf, BUFSIZE, 0);
        pthread_mutex_unlock(&fileLock);
        return;
    }
    int fileSize;
    fseek(fptr, 0, SEEK_END);
    fileSize = ftell(fptr);
    fseek(fptr, 0, SEEK_SET);
    sprintf(buf, "%d", fileSize);
    send(clientfd, buf, BUFSIZE, 0);


    int sent = 0;
    while (sent < fileSize){
        bzero(buf, BUFSIZE);
        int bytesRead = fread(buf, 1, BUFSIZE, fptr);
        if (bytesRead <= 0){
            fprintf(stderr, "Could not read from file\n");
            pthread_mutex_unlock(&fileLock);
            return;
        }
        int bytesSent = send(clientfd, buf, bytesRead, 0);
        if (bytesSent <= 0){
            fprintf(stderr, "Could not send file\n");
            pthread_mutex_unlock(&fileLock);
            return;
        }
        sent+=bytesSent;
    }
    fclose(fptr);
    pthread_mutex_unlock(&fileLock);
    printf("Chunk sent succcessfully\n");
    return;
}

void handlePut(int clientfd, char *msg){
    char buf[BUFSIZE];
    FILE *fptr;
    int n;
    char fileName[100];
    int fileSize;
    int index;

    sscanf(msg, "put %s %d %d", fileName, &index, &fileSize);
    printf("Handling message: %s\n", msg);
   
    sprintf(buf, "./dfs%d/%s", serverNum, fileName);
    printf("Atttempting to access: %s\n", buf);

    struct stat st;
    if (stat(buf, &st) == -1) {
        mkdir(buf, 0777);
        printf("Creating directory: %s\n", buf);
    }

    bzero(buf, BUFSIZE);
    sprintf(buf, "./dfs%d/%s/%d", serverNum, fileName, index);
    printf("Creating path and file: %s\n", buf);
    pthread_mutex_lock(&fileLock);
    fptr = fopen(buf, "wb");
    bzero(buf, BUFSIZE);

    if (!fptr){
        strcpy(buf, "error");
        send(clientfd, buf, BUFSIZE, 0);
        fprintf(stderr, "Could not open file and path\n");
        pthread_mutex_unlock(&fileLock);
        return;
    } else {
        strcpy(buf, "OK");
        n = send(clientfd, buf, BUFSIZE, 0);
        if (n < 0){
            fprintf(stderr, "Error sending OK\n");
            return;
        }
        printf("Sending OK\n");
    }

    int recv = 0;
    do {
        bzero(buf, BUFSIZE);
        n = read(clientfd, buf, BUFSIZE);
        if (n <= 0){
            if (errno == EAGAIN || errno == EWOULDBLOCK){
                printf("Timeout occurred\n");
            } else {
                perror("ERROR on read");
            }
            fclose(fptr);
            pthread_mutex_unlock(&fileLock);
            return;
        }
        n = fwrite(buf, 1, n, fptr);
        if (n <= 0){
            fprintf(stderr, "Could not write to file\n");
            close(sockfd);
            pthread_mutex_unlock(&fileLock);
        }
        recv+=n;
        // printf("RECV %d FILESIZE: %d\n", recv, fileSize);
    } while(recv < fileSize);
    fclose(fptr);
    pthread_mutex_unlock(&fileLock);
    printf("Successfully received file\n");
    return;
}

void handleCheck(int clientfd, char *msg){
    char buf[BUFSMALL];
    char file[100];
    int index;
    FILE *fptr;

    sscanf(msg, "check %s %d", file, &index);
    printf("Handling message: %s\n", msg);

    sprintf(buf, "./dfs%d/%s/%d", serverNum, file, index);
    printf("Attempting to access file: %s\n", buf);
    pthread_mutex_lock(&fileLock);
    fptr = fopen(buf, "rb");
    bzero(buf, BUFSMALL);
    if (!fptr){
        strcpy(buf, "error");
        send(clientfd, buf, BUFSIZE, 0);
        pthread_mutex_unlock(&fileLock);
        return;
    }
    fclose(fptr);
    pthread_mutex_unlock(&fileLock);
    strcpy(buf, "confirm");
    send(clientfd, buf, BUFSMALL, 0);
    return;
}

void *handleClient(void *arg){
    int clientfd = *((int *) arg);
    free(arg);
    char buf[BUFSIZE];
    int n;

    /* Get client message */
    if ((n = read(clientfd, buf, BUFSIZE)) < 0){
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            printf("Timeout occurred\n");
        } else {
            perror("ERROR on read");
        }
        close(clientfd);
        return NULL;
    }

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    if (setsockopt(clientfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) < 0){
        close(clientfd);
        perror("ERROR on sockopt");
        return NULL;
    }

    printf("Server received message: %s\n", buf);

    if (strncmp(buf, "get", 3) == 0){
        handleGet(clientfd, buf);
    }
    else if (strncmp(buf, "put", 3) == 0){
        handlePut(clientfd, buf);
    }
    else if (strncmp(buf, "list", 4) == 0){
        handleList(clientfd);
    }
    else if (strncmp(buf, "check", 5) == 0){
        handleCheck(clientfd, buf);
    }
    // else {
    //     bzero(buf, BUFSIZE);
    //     strcpy(buf, "error");
    //     send(clientfd, buf, BUFSIZE, 0);
    // }

    printf("Thread exiting\n");
    close(clientfd);
    return NULL;
}

int main(int argc, char **argv){
    int portno;   // Socket, port to listen, bytesize of client address
    int *clientfd;
    socklen_t clientlen;
    struct sockaddr_in serveraddr;  // server's addr
    struct sockaddr_in clientaddr;  // client's addr
    struct hostent *hostp;          // client host info
    char *hostaddrp;                // dotted decimal host addr string
    int optval;                     // flag value for setsockopt
    pthread_t tid;

    if (argc != 3){
        fprintf(stderr, "%s ./dfs# [port] &\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[2]);
    if (!portno || portno <= 1024 || portno >= 65535){
        fprintf(stderr, "invalid port number [1025-65524]\n");
        exit(0);

    }

    if (access(argv[1], F_OK) == -1){
        mkdir(argv[1], 0777);
    }

    sscanf(argv[1], "./dfs%d", &serverNum);
    printf("Starting: dfs%d %d\n", serverNum, portno);

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

    if(pthread_mutex_init(&fileLock, NULL) != 0){
        perror("ERROR intializing mutex lock");
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
