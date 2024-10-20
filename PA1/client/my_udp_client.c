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
    for (i=0; i < 20-progress; i++){
        printf("-");
    }
    printf("]");
    fflush(stdout);
    if (size >= maxSize){ printf("\n"); }
}

int sendMsgTo(int sockfd, Message *msg, struct sockaddr_in *serveraddr, socklen_t serverlen){
    int retries = 0;
    int n1;
    int n2;
    Message tempMsg;
    tempMsg.seq = msg->seq;
    memcpy(tempMsg.buf, msg->buf, BUFSIZE);

    /*
    Sending a message:
    1. Copy the message in buf to a temp for storage incase it gets overwritten
    2. Send the message to the server
    3. Wait for an ACK back
        - If we don't get an ACK, continue retrying our send 
        - If we reach our retry limit, maybe the server has crashed, so exit
    */

    msg->seq++;
    while (retries < NUMRETRY){
        n1 = sendto(sockfd, msg, sizeof(Message), 0, (struct sockaddr *)serveraddr, serverlen);
        if (n1 < 0){ error("Error in send to"); } 
        bzero(tempMsg.buf, BUFSIZE);
        n2 = recvfrom(sockfd, &tempMsg, sizeof(Message), 0, (struct sockaddr *)serveraddr, &serverlen);
        if (n2 < 0){
            if (errno == EWOULDBLOCK || errno == EAGAIN){
                /* If we get a timeout then we would reach this spot */
                /* Uncomment for DEBUG */

                // printf("Timeout occurred\n");
            } else {
                error("Error in recvfrom");
            }
        } else {
            if (memcmp(tempMsg.buf, "ACK", 3) == 0){ return n1;}
            else {
                fprintf(stderr, "Unexpected response\n");
                error("SendMsgTo failed");
            }
        }
        retries++;
    }
    printf("Max retries reached. Failed to send message.\n");
    error("SendMsgTo failed");
    return -1;
}

int recvMsgFrom(int sockfd, Message *msg, struct sockaddr_in *serveraddr, socklen_t serverlen, int *previous){
    int retries = 0;
    int n1;
    int n2;
    Message tempMsg;
    tempMsg.seq = msg->seq;
    memcpy(tempMsg.buf, msg->buf, BUFSIZE);

    /* 
    Receiving a message: 
    1. Receive information from the server
        - If we timeout, continue retrying
    2. Send an ACK back
        - If our ACK doesn't reach then we need to check if the server resent their information
        - If they did, resend an ACK, reset the retries and wait for their next response
        - Otherwise, send an ACK and copy the new message as previous
    */

    while (retries < NUMRETRY){
        bzero(msg->buf, BUFSIZE);
        n1 = recvfrom(sockfd, msg, sizeof(Message), 0, (struct sockaddr *)serveraddr, &serverlen);
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
            n2 = sendto(sockfd, &tempMsg, sizeof(Message), 0, (struct sockaddr *)serveraddr, serverlen); // maybe potential for bugs here
            if (n2 < 0) { error("Error in sendto"); }
            if (msg->seq == *previous){
                /* If the client didn't receive our ACK previously, send another and wait for the next response */
                /* Uncomment for DEBUG */
                
                retries = -1; 
                // printf("ACK not received by server\n");
            }
            else { 
                *previous = msg->seq;
                return n1; 
            }
        }
        retries++;
    }
    error("Max retries reached. Failed to receive message");
    return -1;
}

int sendFile(char *command, char *filename, int sockfd, Message *msg, struct sockaddr_in *serveraddr, socklen_t serverlen, int *previous){
    int n;
    FILE *fptr = fopen(filename, "rb");

    /* Attempt to open the file */
    if (fptr == NULL){
        printf("Could not open [File] '%s'\n", filename);
        return 1;
    } else {
    /* If file opens... */
        /* Send our 'put file' command to the server */
        n = sendMsgTo(sockfd, msg, serveraddr, serverlen);
        
        /* Find the file size by seeking to the end of the file and send it to the server */
        if (n < 0) { error("Error in sendto"); }
        bzero(msg->buf, BUFSIZE);
        fseek(fptr, 0, SEEK_END);
        int fileSize = ftell(fptr);
        fseek(fptr, 0, SEEK_SET);
        sprintf(msg->buf, "%d", fileSize);
        n = sendMsgTo(sockfd, msg, serveraddr, serverlen);
        printf("Sending [File] '%s'\nSize: %d\n", filename, fileSize);

        /* Begin sending the file by filling the buffer, sending, clearing the buffer, and repeating */
        int sent = 0;
        while(sent < fileSize){
            bzero(msg->buf, BUFSIZE);
            fread(msg->buf, 1, BUFSIZE, fptr);
            if (ferror(fptr)) error("Error reading file");
            // buf[BUFSIZE-1] = '\0';
            printProgress(sent, fileSize, false);
            n = sendMsgTo(sockfd, msg, serveraddr, serverlen);
            if (n < 0) { error("Error in sendMsgTo"); }
            sent+=BUFSIZE;

            /* Loop will repeat once more so we need to stop it when the file is done sending */
            if (sent >= fileSize) { printProgress(fileSize, fileSize, false); break; } 
        }

        /* Close the file, get server acknowledgement, and log it */
        fclose(fptr);
        bzero(msg->buf, BUFSIZE);
        n = recvMsgFrom(sockfd, msg, serveraddr, serverlen, previous);
        printf("Echo from server: %s\n", msg->buf);
        return 1;
    }
    return -1;
}

int recvFile(char *command, char *filename, int sockfd, Message *msg, struct sockaddr_in *serveraddr, socklen_t serverlen, int *previous){
    int n;

    /* Send our 'get file' command to the server */
    n = sendMsgTo(sockfd, msg, serveraddr, serverlen);
    /* Get the file size back */
    n = recvMsgFrom(sockfd, msg, serveraddr, serverlen, previous);
    
    /* If our requested file does not exist, exit */
    if (strcmp(msg->buf, "File does not exist") == 0){
        printf("Echo from server: %s\n", msg->buf);
        return 1;
    } else {
    /* If the file does exist, */
        /* Save the file size as an int */
        int fileSize = atoi(msg->buf);
        FILE *fptr = fopen(filename, "wb");
        if (ferror(fptr))
            error("Error opening file");
        printf("Receiving [File] '%s'\nSize: %d bytes\n", filename, fileSize);

        int received = 0;
        int toWrite = BUFSIZE;
        while (received < fileSize){
            bzero(msg->buf, BUFSIZE);
            printProgress(received, fileSize, true);
            n = recvMsgFrom(sockfd, msg, serveraddr, serverlen, previous);
            if (received + BUFSIZE > fileSize) { toWrite = fileSize - received; }
            fwrite(msg->buf, 1, toWrite, fptr);
            received+=toWrite;

            /* Loop will repeat once more so we need to stop it when the file size is reached */
            if (received >= fileSize) { printProgress(received, fileSize, true); break; }
        }
        /* Close the file and log it */
        printf("Retrieved file name: %s\nSize: %d bytes\n", filename, received);
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
    int sockfd, portno, n;          // Socket, port to listen, bytesize of client address
    socklen_t serverlen;
    struct sockaddr_in serveraddr;  // server's addr
    struct hostent *server;         // server host info
    char *hostname;                 // ip address
    int previous;                   // prev recved msg
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
    
    /* Set a timeout time for the client */
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = TIMEOUT_T;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
        error("Error setting timeout");

    char command[INPUTSIZE];
    char filename[INPUTSIZE];
    int fileSize = 0;
    int received = 0;
    int sent = 0;

    Message msg; // The message buffer and sequence number

    while (1){
        msg.seq = 0;
        previous = -1;

        /* Clear buffers, and print out prompt to user */
        prompt();
        bzero(msg.buf, BUFSIZE);
        bzero(command, INPUTSIZE);
        bzero(filename, INPUTSIZE);

        /* Copy user arguments and treat as separate strings */
        printf("Please enter a msg: ");
        fgets(msg.buf, INPUTSIZE, stdin);
        sscanf(msg.buf, "%s %s", command, filename);

        if (strcmp(command, "get") == 0 && strlen(filename) > 0){
            /* Move to function that handles receiving files */
            int ret = recvFile(command, filename, sockfd, &msg, &serveraddr, serverlen, &previous);
            if (ret < 0) { error("Error in sendFile"); }
        }
        else if (strcmp(command, "put") == 0 && strlen(filename) > 0){
            /* Move to function that handles sending files */
            int ret = sendFile(command, filename, sockfd, &msg, &serveraddr, serverlen, &previous);
        }
        else if (strcmp(command, "delete") == 0 && strlen(filename) > 0){
            /* Send message to server to delete a file and print out the reply */
            n = sendMsgTo(sockfd, &msg, &serveraddr, serverlen);
            n = recvMsgFrom(sockfd, &msg, &serveraddr, serverlen, &previous);
            printf("Echo from server: %s\n", msg.buf);
        }
        else if (strcmp(command, "ls") == 0){
            /* Send a message to the server to print out its directory and print it out */
            strcpy(msg.buf, "ls");
            n = sendMsgTo(sockfd, &msg, &serveraddr, serverlen);
            if (n < 0){
                error("Error in sendto");
            }
            n = recvMsgFrom(sockfd, &msg, &serveraddr, serverlen, &previous);
            printf("Echo from server:\n%s\n", msg.buf);

        }
        else if (strcmp(command, "exit") == 0 && strlen(filename) <= 0){
            /* Inform the server the client is exiting */
            n = sendMsgTo(sockfd, &msg, &serveraddr, serverlen);
            if (n < 0){
                error("Error in sendto");
            }
            n = recvMsgFrom(sockfd, &msg, &serveraddr, serverlen, &previous);
            printf("Echo from server: %s\n", msg.buf);
            printf(" -- Mischief managed. -- \n");
            return 0;
        }
        else {
            /* Handle wrong input from user */
            if (strlen(filename) <= 0)
                fprintf(stderr, "Command not recognized or missing [filename]\n");
            else {
                msg.buf[strcspn(command, "\n")] = 0;
                fprintf(stderr, "'%s' is not a recognized command.\n", msg.buf);
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