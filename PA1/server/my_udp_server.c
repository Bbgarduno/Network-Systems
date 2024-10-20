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

#define BUFSIZE 1024
#define INPUTSIZE 200
#define NUMRETRY 10
#define TIMEOUT_T 300000

typedef struct {
    int seq;
    char buf[BUFSIZE];
} Message;

void error(char *msg){
    perror(msg);
    exit(1);
}

void printProgress(int size, int maxSize, bool recv){
    /* Print information regarding the status of sending/receiving a file */
    /* Save the progress a fraction of 20 --> x/20 */
    int progress = (int)((double)size/maxSize * 20);
    int i;
    printf("\r%d/%d bytes ", size, maxSize);
    if (recv){ printf("received ["); }
    else { printf("sent ["); }
    
    for(i = 0; i < progress; i++){
        printf("#");
    }
    for (i = 0; i < 20-progress; i++){
        printf("-");
    }
    printf("]");
    fflush(stdout);
    if (size >= maxSize){ printf("\n"); }
}

int sendMsgTo(int sockfd, Message *msg, struct sockaddr_in *clientaddr, socklen_t clientlen){
    int retries = 0;
    int n1;
    int n2;
    Message tempMsg;
    tempMsg.seq = msg->seq;
    memcpy(tempMsg.buf, msg->buf, BUFSIZE);

    /*
    Sending a message:
    1. Copy the message in buf to a temp for storage incase it gets overwritten
    2. Send the message to the client
    3. Wait for an ACK back
        - If we don't get an ACK, continue retrying our send 
        - If we reach our retry limit, maybe the client is gone, so exit
    */
    msg->seq++;
    while (retries < NUMRETRY){
        n1 = sendto(sockfd, msg, sizeof(Message), 0, (struct sockaddr *)clientaddr, clientlen);
        if (n1 < 0){ error("Error in send to"); } 
        bzero(tempMsg.buf, BUFSIZE);
        n2 = recvfrom(sockfd, &tempMsg, sizeof(Message), 0, (struct sockaddr *)clientaddr, &clientlen);
        if (n2 < 0){
            if (errno == EWOULDBLOCK || errno == EAGAIN){
                /* If we get a timeout then we would reach this spot */
                /* Uncomment for DEBUG */

                // printf("Timeout occurred\n");
            } else {
                error("Error in recvfrom");
            }
        } else {
            if (memcmp(tempMsg.buf, "ACK", 3) == 0) { return n1; }
            else {
                fprintf(stderr, "Unexpected response\n");
                error("SendMsgTo failed");
            }
        }
        retries++;
    }
    printf("Max retries reached. Failed to send message.\n");
    return -1;
}

int recvMsgFrom(int sockfd, Message *msg, struct sockaddr_in *clientaddr, socklen_t clientlen, int *previous){
    int retries = 0;
    int n1;
    int n2;
    Message tempMsg;
    tempMsg.seq = msg->seq;
    memcpy(tempMsg.buf, msg->buf, BUFSIZE);

    /* 
    Receiving a message: 
    1. Receive information from the client
        - If we timeout, continue retrying
    2. Send an ACK back
        - If our ACK doesn't reach then we need to check if the client resent their information
        - If they did, resend an ACK, reset the retries and wait for their next response
        - Otherwise, send an ACK and copy the new message as previous
    */

    while (retries < NUMRETRY){
        bzero(msg->buf, BUFSIZE);
        n1 = recvfrom(sockfd, msg, sizeof(Message), 0, (struct sockaddr *)clientaddr, &clientlen);
        if (n1 < 0){
            if (errno == EWOULDBLOCK || errno == EAGAIN){
                /* If we get a timeout then we would reach this spot */
                /* Uncomment for DEBUG */

                // printf("Timeout occurred\n");
            } else {
                error("Error in recvfrom");
            }
        } else {
            bzero(tempMsg.buf, BUFSIZE);
            memcpy(tempMsg.buf, "ACK", 3);
            n2 = sendto(sockfd, &tempMsg, sizeof(Message), 0, (struct sockaddr *)clientaddr, clientlen); // maybe potential for bugs here
            if (n2 < 0) { error("Error in sendto"); }
            if (msg->seq == *previous){
                /* If the client didn't receive our ACK previously, send another and wait for the next response */
                /* Uncomment for DEBUG */

                retries = -1; 
                // printf("ACK not received by client\n");
            }
            else { 
                *previous = msg->seq;
                return n1; 
            }
        }
        retries++;
    }
    printf("Max retries reached. Failed to receive message\n");
    return -1;
}

int recvFile(char *command, char *filename, int sockfd, Message *msg, struct sockaddr_in *clientaddr, socklen_t clientlen, int *previous){
    bzero(msg->buf, BUFSIZE);
    int n;

    /* Receive the file size from the client and attempt to open file */
    n = recvMsgFrom(sockfd, msg, clientaddr, clientlen, previous);
    if (n < 0){ printf("Stopping current operation\n"); return 0; }
    int fileSize = atoi(msg->buf);
    FILE *fptr = fopen(filename, "wb");
    if (ferror(fptr)) { error("Error opening file"); }
    printf("Receiving [File] '%s'\nSize: %d bytes\n", filename, fileSize);

    /* If file opens... */
    /* Begin receiving the file by filling the buffer, writing to a file, clearing the buffer, and repeating */
    int received = 0;
    int toWrite = BUFSIZE;
    while (received < fileSize){
        bzero(msg->buf, BUFSIZE);
        printProgress(received, fileSize, true);
        n = recvMsgFrom(sockfd, msg, clientaddr, clientlen, previous);

        if (n < 0){ printf("Stopping current operation\n"); return 0; }
        if (received + BUFSIZE > fileSize) { toWrite = fileSize - received; }
        fwrite(msg->buf, 1, toWrite, fptr);
        received+=toWrite;

        /* Loop will repeat once more so we need to stop it when the file size is reached */
        if (received >= fileSize) { printProgress(received, fileSize, true); break; }
    }
    /* Close the file, log it, and inform client */
    fclose(fptr);
    printf("Received [File] '%s'\n", filename);
    bzero(msg->buf, BUFSIZE);
    sprintf(msg->buf, "Success: [File] '%s' received", filename);
    n = sendMsgTo(sockfd, msg, clientaddr, clientlen);
    if (n < 0){ printf("Stopping current operation\n"); return 0; }

    return 1;
}

int sendFile(char *command, char *filename, int sockfd, Message *msg, struct sockaddr_in *clientaddr, socklen_t clientlen, int *previous){
    int n;
    bzero(msg->buf, BUFSIZE);

    /* Attempt to open file */
    FILE *fptr = fopen(filename, "rb");
    if (!fptr){
        strcpy(msg->buf, "File does not exist");
        n = sendMsgTo(sockfd, msg, clientaddr, clientlen);
        if (n < 0){ printf("Stopping current operation\n"); return 0; }
        printf("sent response: file not found\n");
        return 1;
    } else {
    /* If file opens... */
        /* Find the file size by seeking to the end of the file and send it to the client */
        bzero(msg->buf, BUFSIZE);
        fseek(fptr, 0, SEEK_END);
        int fileSize = ftell(fptr);
        fseek(fptr, 0, SEEK_SET);
        sprintf(msg->buf, "%d", fileSize);
        n = sendMsgTo(sockfd, msg, clientaddr, clientlen);
        if (n < 0){ printf("Stopping current operation\n"); return 0; }
        
        /* Begin sending the file by filling the buffer, sending, clearing the buffer, and repeating */
        int sent = 0;
        printf("Sending [File] '%s'\nSize: %d bytes\n", filename, fileSize);
        while (sent < fileSize){
            bzero(msg->buf, BUFSIZE);
            fread(msg->buf, 1, BUFSIZE, fptr);
            // buf[BUFSIZE-1] = '\0';
            printProgress(sent, fileSize, false);
            n = sendMsgTo(sockfd, msg, clientaddr, clientlen);
            sent+=BUFSIZE;
            if (n < 0){ printf("Stopping current operation\n"); return 0; }
            
            /* Loop will repeat once more so we need to stop it when the file is done sending */
            if (sent >= fileSize) { printProgress(fileSize, fileSize, false); break; } 
        }
        /* Close file and log it */
        fclose(fptr);
        printf("[File] '%s' sent\n", filename);
        return 1;
    }
    return -1;
}

int main(int argc, char **argv){
    int sockfd, portno;             // Socket, port to listen, bytesize of client address
    socklen_t clientlen;
    struct sockaddr_in serveraddr;  // server's addr
    struct sockaddr_in clientaddr;  // client's addr
    struct hostent *hostp;          // client host info
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
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = TIMEOUT_T;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
        error("Error setting timeout");
        
    printf("Server is now listening on port %d\n", portno);

    Message msg; // The message buffer and sequence number

    while(1){
        msg.seq = 0;
        previous = -1;

        // Clear buffers
        bzero(msg.buf, BUFSIZE);

        /* Allow no timeouts when listening for a user's first message */
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
        if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
            error("Error setting timeout");

        n = recvMsgFrom(sockfd, &msg, &clientaddr, clientlen, &previous);

        /* Next message comes automatically so allow timeouts */
        timeout.tv_sec = 0;
        timeout.tv_usec = TIMEOUT_T;
        if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
            error("Error setting timeout");

        // hostp = gethostbyaddr((const char *) &clientaddr.sin_addr.s_addr, sizeof(clientaddr.sin_addr.s_addr), AF_INET);
        // if (hostp == NULL){
        //     error("Error on gethostbyaddr");
        // }
        hostaddrp = inet_ntoa(clientaddr.sin_addr);
        if (hostaddrp == NULL)
            error("Error on inet_ntoa");
        // printf("server received datagram from %s (%s)\n", hostp->h_name, hostaddrp);
        printf("server received datagram from (%s)\n", hostaddrp);
        
        /* Copy user arguments and treat as separate strings */
        sscanf(msg.buf, "%s %s", command, filename);
        command[INPUTSIZE-1] = '\0';
        filename[INPUTSIZE-1] = '\0';
        
        if (strcmp(command, "get") == 0 && strlen(filename) > 0){
            /* Move to function that handles sending files */
            int ret = sendFile(command, filename, sockfd, &msg, &clientaddr, clientlen, &previous);
            if (ret < 0) { error("Error on sendFile"); }
        }
        else if (strcmp(command, "put") == 0 && strlen(filename) > 0){
            /* Move to function that handles receiving files */
            int ret = recvFile(command, filename, sockfd, &msg, &clientaddr, clientlen, &previous);
            if (ret < 0) { error("Error on recvFile"); }
        }
        else if (strcmp(command, "delete") == 0 && strlen(filename) > 0){
            /* Make a meaningful message to know what file was deleted */
            int ret = remove(filename);
            if (ret == 0){
                sprintf(msg.buf, "[File] '%s' deleted succesfully", filename);
                printf("[File] '%s' deleted\n", filename);
            } else {
                sprintf(msg.buf, "[File] '%s' could not be deleted", filename);
                printf("Failed to delete a file\n");
            }

            n = sendMsgTo(sockfd, &msg, &clientaddr, clientlen);
            if (n < 0){ printf("Stopping current operation\n"); }
        }
        else if (strcmp(command, "ls") == 0){
            /* Send all file names in directory to client */
            bzero(msg.buf, BUFSIZE);
            DIR *d;
            struct dirent *dir;
            d = opendir(".");
            if (d){
                while((dir = readdir(d)) != NULL){
                    // if (dir->d_type == DT_REG && strstr(dir->d_name, ".txt") != NULL){
                    /* Scan directory for all files excluding folders */
                    if (dir->d_type == DT_REG){
                        strcat(msg.buf, dir->d_name);
                        strcat(msg.buf, "\n");
                    }
                }
                closedir(d);
                if (strlen(msg.buf) == 0)
                    strcpy(msg.buf, "{no files found}");
            } else {
                error("Error opening directory");
            }

            n = sendMsgTo(sockfd, &msg, &clientaddr, clientlen);
            if (n < 0){ printf("Stopping current operation\n"); }
            else { printf("Sent directory\n"); }
        }
        else if (strcmp(command, "exit") == 0){
            /* Send client a goodbye message */
            bzero(msg.buf, BUFSIZE);
            strcpy(msg.buf, "Goodbye!");
            n = sendMsgTo(sockfd, &msg, &clientaddr, clientlen);
            if (n < 0){ printf("Stopping current operation\n"); }
            else { printf("(%s) exiting\n", hostaddrp); }
        }
        else {
            /* Deal with unrecognized commands that manage to come through */
            bzero(msg.buf, BUFSIZE);
            strcpy(msg.buf, "Command not recognized or missing filename");
            n = sendto(sockfd, &msg, strlen(msg.buf), 0, (struct sockaddr*) &clientaddr, clientlen);
            if (n < 0){
                error("Error on sendto");
            }
        }
    }
}
