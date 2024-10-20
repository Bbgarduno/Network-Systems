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

#define BUFSIZE 1024
#define INPUTSIZE 200
#define NUMRETRY 3

int sendMsgTo(int sockfd, char *buf, struct sockaddr_in *serveraddr, socklen_t serverlen){
    int retries = 0;
    char tempBuf[BUFSIZE];
    strcpy(tempBuf, buf);

    while (retries <= NUMRETRY){
        int n = sendto(sockfd, tempBuf, strlen(buf), 0, (struct sockaddr *)serveraddr, serverlen);
        if (n < 0) { error("Error in sendto"); }
        n = recvfrom(sockfd, buf, BUFSIZE, 0, (struct sockaddr *)serveraddr, &serverlen);
        if (n < 0){
            if (errno == EWOULDBLOCK || errno == EAGAIN){
                printf("Timeout occurred\n");
            } else {
                error("Error in recvfrom");
            }
        } else {
            if (strcmp(buf, "ACK") != 0)  
                error("Did not receive ACK");
        }
        retries++;
    }
    return -1;
}

int recvMsgFrom(int sockfd, char *buf, struct sockaddr_in *serveraddr, socklen_t serverlen){
    int retries = 0;
    while (retries < NUMRETRY){
        int n = recvfrom(sockfd, buf, BUFSIZE, 0, (struct sockaddr *)serveraddr, &serverlen);
        if (n < 0){
            if (errno == EWOULDBLOCK || errno == EAGAIN){
                printf("Timeout occurred\n");
            } else {
                error("Error in recvfrom");
            }
        } else {
            // int n = sendto(sockfd, "ACK", 3, 0, serveraddr, serverlen); // What happens if ACK gets lost?
            // if (n < 0) { error("Error in sendto"); }
            return 1;
        }
    }
    return -1;
}

int recvFile(char *command, char* filename, char *buf, int sockfd, struct sockaddr_in *clientaddr, socklen_t clientlen){
    int n;
    char prevBuf[BUFSIZE];
    bzero(buf, BUFSIZE);
    bzero(prevBuf, BUFSIZE);
    strcpy(buf, "ACK");
    n = sendMsgTo(sockfd, buf, clientaddr, clientlen); // Send ACK and rcv file size
    if (n < 0) { error("Error in sendMsgTo"); }
    else {                                                                                 // case 2 server respones - file size
        int fileSize = atoi(buf);
        FILE *fptr = fopen(filename, "w");
        if (ferror(fptr))
            error("Error opening file");

        int received = 0;
        bzero(prevBuf, BUFSIZE);
        while (received < fileSize){
            bzero(buf, BUFSIZE);
            if (strcmp(buf, prevBuf) != 0){
                n = recvMsgFrom(sockfd, buf, clientaddr, clientlen);
                strcpy(prevBuf, buf);
                fwrite(buf, 1, strlen(buf), fptr);
                received+=strlen(buf);
            }
            if (received == fileSize) break;
            bzero(buf, BUFSIZE);
            strcpy(buf, "ACK");
            int n = sendto(sockfd, buf, strlen(buf), 0, (struct sockaddr *)serveraddr, serverlen);
        }
        printf("Retrieved file name: %s\nLength: %d bytes\n", filename, received);
        fclose(fptr);
        return 1;
    }
    return -1;
}

int sendFile(char *command, char* filename, char *buf, int sockfd, struct sockaddr_in *clientaddr, socklen_t clientlen){
    int fileSize, sent, n;
    bzero(buf, BUFSIZE);
    FILE *fptr = fopen(filename, "r");
    if (!fptr){
         strcpy(buf, "File does not exist");
         n = sendto(sockfd, buf, strlen(buf), 0, (struct sockaddr*) &clientaddr, clientlen);
         return 1; 
    } 
    else {
        fseek(fptr, 0, SEEK_END);
        fileSize = ftell(fptr);
        fseek(fptr, 0, SEEK_SET);
        sprintf(buf, "%d", fileSize);
        n = sendMsgTo(sockfd, buf, clientaddr, clientlen); // Send file size and recv ACK

        sent = 0;
        printf("Sending [File] '%s'", filename);
        while(sent < fileSize){
            bzero(buf, BUFSIZE);
            fread(buf, BUFSIZE-1, 1, fptr);
            if (ferror(fptr)) { error("Error reading file"); }
            buf[BUFSIZE-1] = '\0';
            n = sendMsgTo(sockfd, buf, clientaddr, clientlen); // Send part of file and recv ACK
            if (n < 0) { error("Error on sendMsgTo"); }
            sent+=strlen(buf);
            printf("%d/%d bytes sent\n", sent, fileSize);
            if (sent == fileSize) break;
        }
        fclose(fptr);
        return 1;
    }
    return -1;
}

void error(char *msg){
    perror(msg);
    exit(1);
}

int main(int argc, char **argv){
    int sockfd, portno, clientlen; // Socket, port to listen, bytesize of client address
    struct sockaddr_in serveraddr; // server's addr
    struct sockaddr_in clientaddr; // client's addr
    struct hostent *hostp; // client host info
    char buf[BUFSIZE]; // message 
    char command[INPUTSIZE];
    char filename[INPUTSIZE];
    char *hostaddrp; // dotted decimal host addr string
    int optval; // flag value for setsockopt
    int n; // message byte size
    FILE *fptr;

    int sent = 0;
    int received = 0;
    int fileSize = 0;

    if (argc != 2){
        fprintf(stderr, "%s [port]\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);
    if (!portno || portno <= 1024 || portno >= 65535){
        fprintf(stderr, "invalid port number [1025-65524]\n");
        exit(0);

    }
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0){
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
    while(1){
        bzero(buf, BUFSIZE);
        n = recvfrom(sockfd, buf, BUFSIZE, 0, (struct sockaddr*) &clientaddr, &clientlen);
        if (n < 0)
            error("Error in recvfrom");
        // hostp = gethostbyaddr((const char *) &clientaddr.sin_addr.s_addr, sizeof(clientaddr.sin_addr.s_addr), AF_INET);
        // if (hostp == NULL){
        //     error("Error on gethostbyaddr");
        // }
        hostaddrp = inet_ntoa(clientaddr.sin_addr);
        if (hostaddrp == NULL)
            error("Error on inet_ntoa");
        // printf("server received datagram from %s (%s)\n", hostp->h_name, hostaddrp);
        printf("server received datagram from (%s)\n", hostaddrp);
        
        sscanf(buf, "%s %s", command, filename);
        command[INPUTSIZE-1] = '\0';
        filename[INPUTSIZE-1] = '\0';

        if (strcmp(command, "get") == 0 && strlen(filename) > 0){
            int ret = sendFile(command, filename, buf, sockfd, &clientaddr, clientlen);
            if (ret < 0) { error("Error on sendFile"); }
            // bzero(buf, BUFSIZE);
            // fptr = fopen(filename, "r");
            // if (!fptr){
            //     strcpy(buf, "File does not exist");
            //     n = sendto(sockfd, buf, strlen(buf), 0, (struct sockaddr*) &clientaddr, clientlen);
            //     continue;
            // }
            // else {
            //     fseek(fptr, 0, SEEK_END);
            //     fileSize = ftell(fptr);
            //     fseek(fptr, 0, SEEK_SET);
            //     sprintf(buf, "%d", fileSize);
            //     n = sendto(sockfd, buf, strlen(buf), 0, (struct sockaddr*) &clientaddr, clientlen);
            //     if (n < 0)
            //         error("Error on send to");
                
            //     sent = 0;
            //     printf("Sending [File] '%s'\n", filename);
            //     while (sent < fileSize){
            //         bzero(buf, BUFSIZE);
            //         fread(buf, BUFSIZE-1, 1, fptr);
            //         buf[BUFSIZE-1] = '\0';
            //         n = sendto(sockfd, buf, strlen(buf), 0, (struct sockaddr*) &clientaddr, clientlen);
            //         if (n < 0)
            //             error("Error on send to");
            //         sent+=strlen(buf);
            //         printf("%d/%d bytes sent\n", sent, fileSize);
            //         if (sent == fileSize) break;
            //     }
            //     fclose(fptr);
            // }
        }
        else if (strcmp(command, "put") == 0 && strlen(filename) > 0){
            bzero(buf, BUFSIZE);
            n = recvfrom(sockfd, buf, BUFSIZE, 0, (struct sockaddr*) &clientaddr, &clientlen);
            if (n < 0)
                error("Error on recvfrom");
            fileSize = atoi(buf);
            fptr = fopen(filename, "w");
            if (ferror(fptr))
                error("Error opening file");
            
            received = 0;
            while (received < fileSize){
                bzero(buf, BUFSIZE);
                n = recvfrom(sockfd, buf, BUFSIZE, 0, (struct sockaddr*) &clientaddr, &clientlen);
                fwrite(buf, 1, strlen(buf), fptr);
                received+=strlen(buf);
                if (received == fileSize) break;
            }
            fclose(fptr);
            printf("Recevied [File] '%s'\n", filename);
            bzero(buf, BUFSIZE);
            sprintf(buf, "Success: [File] '%s' received", filename);
            n = sendto(sockfd, buf, strlen(buf), 0, (struct sockaddr*) &clientaddr, clientlen);
            if (n < 0)
                error("Error on sendto");
        }
        else if (strcmp(command, "delete") == 0 && strlen(filename) > 0){
            int ret = remove(filename);
            if (ret == 0){
                sprintf(buf, "[File] '%s' deleted succesfully", filename);
                printf("[File] '%s' deleted\n", filename);
            } else {
                sprintf(buf, "[File] '%s' could not be deleted", filename);
            }
            n = sendto(sockfd, buf, strlen(buf), 0, (struct sockaddr*) &clientaddr, clientlen);
            if (n < 0)
                error("Error on sendto");
        }
        else if (strcmp(command, "ls") == 0){
            bzero(buf, BUFSIZE);
            DIR *d;
            struct dirent *dir;
            d = opendir(".");
            if (d){
                while((dir = readdir(d)) != NULL){
                    // if (dir->d_type == DT_REG && strstr(dir->d_name, ".txt") != NULL){
                    if (dir->d_type == DT_REG){
                        strcat(buf, dir->d_name);
                        strcat(buf, "\n");
                    }
                }
                closedir(d);
                if (strlen(buf) == 0)
                    strcpy(buf, "{no files found}");
            } else {
                error("Error opening directory");
            }

            n = sendto(sockfd, buf, strlen(buf), 0, (struct sockaddr*) &clientaddr, clientlen);
            if (n < 0)
                error("Error with send to");
            printf("Sent directory\n");
        }
        else if (strcmp(command, "exit") == 0){
            bzero(buf, BUFSIZE);
            strcpy(buf, "Goodbye!");
            n = sendto(sockfd, buf, strlen(buf), 0, (struct sockaddr*) &clientaddr, clientlen);
            if (n < 0)
                error("Error in sendto");
            printf("(%s) exiting\n", hostaddrp);
        }
        else {
            bzero(buf, BUFSIZE);
            strcpy(buf, "Command not recognized or missing filename");
            n = sendto(sockfd, buf, strlen(buf), 0, (struct sockaddr*) &clientaddr, clientlen);
            if (n < 0){
                error("Error on sendto");
            }
        }

        // n = sendto(sockfd, buf, strlen(buf), 0, (struct sockaddr*) &clientaddr, clientlen);
        // if (n < 0)
        //     error("Error in sendto");
    }
}
