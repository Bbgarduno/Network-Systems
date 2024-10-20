#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <sys/time.h>
#include <errno.h>

#define BUFSIZE 1024
#define INPUTSIZE 200
#define NUMRETRY 3

void error(char *msg){
    perror(msg);
    exit(1);
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

// Send message and get an ACK back - replaces sendto()
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
            return 1;
            // if (strcmp(buf, "ACK") == 0)
            //     return 1;
            // else    
            //     error("Did not receive ACK");
        }
        retries++;
    }
    return -1;
}

int sendFile(char *command, char* filename, char *buf, int sockfd, struct sockaddr_in *serveraddr, socklen_t serverlen){
    int fileSize, sent;
    FILE *fptr = fopen(filename, "r");
    if (fptr == NULL){ printf("Could not open [File] '%s'\n", filename); return 1; } 
    else {
        int n = sendMsgTo(sockfd, buf, serveraddr, serverlen); // Send put file and recv ACK
        if (n < 0) { error("Error sending packet"); }

        fseek(fptr, 0, SEEK_END);
        fileSize = ftell(fptr);
        fseek(fptr, 0, SEEK_SET);
        sprintf(buf, "%d", fileSize);
        n = sendMsgTo(sockfd, buf, serveraddr, serverlen); // Send file size and recv ACK

        sent = 0;
        while(sent < fileSize){
            bzero(buf, BUFSIZE);
            fread(buf, BUFSIZE-1, 1, fptr);
            if (ferror(fptr)) error("Error reading file");
            buf[BUFSIZE-1] = '\0';
            n = sendMsgTo(sockfd, buf, serveraddr, serverlen); // Send part of file and recv ACK
            if (n < 0)
                error("Error on send to");
            sent+=strlen(buf);
            // if (strlen(buf) > 0){printf("we got here1\n"); break;}
            // if (strlen(buf) == 0){printf("we got here2\n"); break;}
            printf("%d/%d bytes sent\n", sent, fileSize);
            if (sent == fileSize) break;
        }

        fclose(fptr);
        // Server's reply
        // bzero(buf, BUFSIZE);
        // n = recvfrom(sockfd, buf, BUFSIZE, 0, &serveraddr, &serverlen);
        // if (n < 0){
        //     error("Error in recvfrom");
        // }
        printf("Echo from server: %s\n", buf);
        return 1;
    }
    return -1;
}

int recvFile(char *command, char* filename, char *buf, int sockfd, struct sockaddr_in *serveraddr, socklen_t serverlen){
    int n;
    char prevBuf[BUFSIZE];
    n = sendMsgTo(sockfd, buf, serveraddr, serverlen); // Send get file
    if (n < 0) { error("Error sending packet"); }
    if (strcmp(buf, "File does not exist") == 0){ printf("Echo from server: %s\n", buf); } // case 1 server response - file dne
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
                n = recvMsgFrom(sockfd, buf, serveraddr, serverlen);
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

void prompt(){
    printf("..................................\n");
    printf("--Commands--\n");
    printf("\t'get [file_name]'\n");
    printf("\t'put [file_name]'\n");
    printf("\t'delete [file_name]'\n");
    printf("\t'ls'\n");
    printf("\t'exit'\n");
    printf("..................................\n");
}

int main(int argc, char **argv) {
    int sockfd, portno, n;
    socklen_t serverlen;
    struct sockaddr_in serveraddr;
    struct hostent *server;
    char *hostname;
    char buf[BUFSIZE];
    FILE *fptr;

    if (argc != 3){
        fprintf(stderr, "%s [ip_address] [port_number]\n", argv[0]);
        exit(0);
    }

    hostname = argv[1];
    portno = atoi(argv[2]);
    if (!portno || portno <= 1024 || portno >= 65535){
        fprintf(stderr, "Invalid port number [1025-65534]\n");
        exit(0);
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0){
        perror("Error opening socket");
        exit(0);
    }

    server = gethostbyname(hostname);
    if (server == NULL){
        fprintf(stderr, "Error: No such host as %s", hostname);
        exit(0);
    }

    bzero((char  *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *) server->h_addr, (char *) &serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);
    serverlen = sizeof(serveraddr);
    
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
        error("Error setting timeout");

    char command[INPUTSIZE];
    char filename[INPUTSIZE];

    while (1){
        prompt();
        bzero(buf, BUFSIZE);
        bzero(command, INPUTSIZE);
        bzero(filename, INPUTSIZE);
        printf("Please enter a msg: ");
        fgets(buf, INPUTSIZE, stdin);
        sscanf(buf, "%s %s", command, filename);

        if (strcmp(command, "get") == 0 && strlen(filename) > 0){
            int ret = recvFile(command, filename, buf, sockfd, &serveraddr, serverlen);
            if (ret < 0){ error("Could not get file"); }
            // Send msg to server
            // n = sendMsgTo(sockfd, buf, &serveraddr, serverlen);
            // if (n < 0){
            //     continue;
            // } 

            // // Server's reply
            // n = recvfrom(sockfd, buf, BUFSIZE, 0, &serveraddr, &serverlen);
            // if (n < 0){
            //     error("Error in recvfrom");
            // }
            // // Received file
            // if (strcmp(buf, "File does not exist") == 0){
            //     printf("Echo from server: %s\n", buf);
            //     continue;
            // } else {
            //     // Receive one buf at a time from the server
            //     // First receive expected size, then 1 buf at a time
            //     fileSize = atoi(buf);
            //     // printf("File size: %d\n", fileSize);
            //     fptr = fopen(filename, "w");
            //     if (ferror(fptr))
            //         error("Error opening file");

            //     received = 0;
            //     while (received < fileSize){
            //         bzero(buf, BUFSIZE);
            //         n = recvfrom(sockfd, buf, BUFSIZE, 0, &serveraddr, &serverlen);
            //         fwrite(buf, 1, strlen(buf), fptr);
            //         received+=strlen(buf);
            //         if (received == fileSize) break;
            //     }
            //     printf("Retrieved file name: %s\nLength: %d bytes\n", filename, received);
            //     fclose(fptr);
            // }
        }
        else if (strcmp(command, "put") == 0 && strlen(filename) > 0){
            int ret = sendFile(command, filename, buf, sockfd, &serveraddr, serverlen);
            if (ret < 0){ error("Could not put file"); }
            // Send to server
            // fptr = fopen(filename, "r");
            // if (fptr == NULL){
            //     // error("Error opening file");
            //     printf("Could not open [File] '%s'\n", filename);
            // } else {
            //     n = sendMsgTo(sockfd, buf, &serveraddr, serverlen);
            //     if (n < 0)
            //         error("Error in sendto");

            //     fseek(fptr, 0, SEEK_END);
            //     fileSize = ftell(fptr);
            //     fseek(fptr, 0, SEEK_SET);
            //     sprintf(buf, "%d", fileSize);
            //     n = sendMsgTo(sockfd, buf, &serveraddr, serverlen);

            //     sent = 0;
            //     while(sent < fileSize){
            //         bzero(buf, BUFSIZE);
            //         fread(buf, BUFSIZE-1, 1, fptr);
            //         if (ferror(fptr)) error("Error reading file");
            //         buf[BUFSIZE-1] = '\0';
            //         n = sendMsgTo(sockfd, buf, &serveraddr, serverlen);
            //         if (n < 0)
            //             error("Error on send to");
            //         sent+=strlen(buf);
            //         // if (strlen(buf) > 0){printf("we got here1\n"); break;}
            //         // if (strlen(buf) == 0){printf("we got here2\n"); break;}
            //         printf("%d/%d bytes sent\n", sent, fileSize);
            //         if (sent == fileSize) break;
            //     }

            //     fclose(fptr);
            //     // Server's reply
            //     bzero(buf, BUFSIZE);
            //     n = recvfrom(sockfd, buf, BUFSIZE, 0, &serveraddr, &serverlen);
            //     if (n < 0){
            //         error("Error in recvfrom");
            //     }
            //     printf("Echo from server: %s\n", buf);
            // }
        }
        else if (strcmp(command, "delete") == 0 && strlen(filename) > 0){
            // Send msg to server
            n = sendMsgTo(sockfd, buf, &serveraddr, serverlen);
            if (n < 0){
                error("Error in sendto");
            }

            // Server's reply
            n = recvfrom(sockfd, buf, BUFSIZE, 0, (struct sockaddr *)&serveraddr, &serverlen);
            if (n < 0){
                error("Error in recvfrom");
            }
            printf("Echo from server: %s\n", buf);
        }
        else if (strcmp(command, "ls") == 0){
            strcpy(buf, "ls");
            // Send msg to server
            n = sendMsgTo(sockfd, buf, &serveraddr, serverlen);
            if (n < 0){
                error("Error in sendto");
            }

            // Server's reply
            n = recvfrom(sockfd, buf, BUFSIZE, 0, (struct sockaddr *)&serveraddr, &serverlen);
            if (n < 0){
                error("Error in recvfrom");
            }
            printf("Echo from server:\n%s\n", buf);

        }
        else if (strcmp(command, "exit") == 0 && strlen(filename) <= 0){
            // Send msg to server
            n = sendMsgTo(sockfd, buf, &serveraddr, serverlen);
            if (n < 0){
                error("Error in sendto");
            }

            // Server's reply
            // n = recvfrom(sockfd, buf, BUFSIZE, 0, (struct sockaddr *)&serveraddr, &serverlen);
            // if (n < 0){
            //     error("Error in recvfrom");
            // }
            printf("Echo from server: %s\n", buf);

            printf(" -- Mischief managed. -- \n");
            return 0;
        }
        else {
            if (strlen(filename) <= 0)
                fprintf(stderr, "Command not recognized or missing [filename]\n");
            else {
                buf[strcspn(command, "\n")] = 0;
                fprintf(stderr, "'%s' is not a recognized command.\n", buf);
            }
            fflush(stdin);
        }
    }

    // // Send msg to server
    // n = sendto(sockfd, buf, strlen(buf), 0, &serveraddr, serverlen);
    // if (n < 0){
    //     perror("Error in sendto");
    //     exit(0);
    // } 

    // // Server's reply
    // n = recvfrom(sockfd, buf, strlen(buf), 0, &serveraddr, &serverlen);
    // if (n < 0){
    //     perror("Error in recvfrom");
    //     exit(0);
    // }
    // printf("Echo from server: %s", buf);

    return 0;
}