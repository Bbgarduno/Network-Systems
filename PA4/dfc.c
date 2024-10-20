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
int numServer = 0;

struct dfcConf{
    char server[4][INET_ADDRSTRLEN];
    int port[4];
    int sockfd[4];
};

int connectToServer(char *address, int portno){
    int sockfd = -1;
    int optval = 1;
    struct sockaddr_in serveraddr;
    struct hostent *hostp; 

    hostp = gethostbyname(address);
    if (!hostp){
        fprintf(stderr, "Could not resolve host address\n");
        return -1;
    }
    
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = *(in_addr_t*)hostp->h_addr;
    serveraddr.sin_port = htons((unsigned short) portno);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0){
        fprintf(stderr, "Could not create socket\n");
        return -1;   
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *) &optval, sizeof(int)) < 0){
        perror("ERROR with setsockopt");
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0){
        // perror("ERROR using connect");
        return -1;
    }

    return sockfd;
}

void closeServer(int sockfd){
    if (sockfd > 0){
        close(sockfd);
    }
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

int getChunk(char *file, int chunkIndex, int sockfd, FILE *fptr){
    char buf[BUFSIZE];
    char md5[MD5_DIGEST_LENGTH*2 + 1];
    computeMD5(file, md5);

    // snprintf(buf, BUFSIZE, "get %s %d", md5, chunkIndex);
    snprintf(buf, BUFSIZE, "get %s %d", file, chunkIndex);
    printf("%s\n", buf);
    int n = send(sockfd, buf, BUFSIZE, 0);
    if (n <= 0){ 
        fprintf(stderr, "Could not send prompt\n");
        return -1; 
    }
    bzero(buf, BUFSIZE);

    struct timeval tv;  
    int fileSize;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) < 0)
        perror("ERROR on sockopt rcvtimeo");

    if ((n = read(sockfd, buf, BUFSIZE)) < 0){
        fprintf(stderr, "Server unavailable could not send chunk\n");
        return -1;
    }
    if (strcmp(buf, "error") == 0){
        fprintf(stderr, "Could not get chunk from server\n");
        return -1;
    } else {
        fileSize = atoi(buf);
    }
    
    bzero(buf, BUFSIZE);

    int recv = 0;
    while (recv < fileSize){
        int bytesRecv = read(sockfd, buf, BUFSIZE);
        if (n <= 0){
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("Timeout occurred\n");
            } else {
                perror("ERROR on read");
            }
            fprintf(stderr, "Could not receive chunk\n");
            return -1;
        }
        n = fwrite(buf, 1, bytesRecv, fptr);
        if (n <= 0){
            fprintf(stderr, "Could not write to file\n");
            return -1;
        }
        recv+=n;
        bzero(buf, BUFSIZE);
    }
    close(sockfd);
    printf("Chunk received successfully\n");
    return 1;
}

int sendChunk(int sockfd, char *file, int partSize, int index, FILE *fptr){
    char buf[BUFSIZE];
    int n;
    int totalSize = partSize;
    struct timeval tv;  

    tv.tv_sec = 2;
    tv.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) < 0){
        perror("ERROR on sockopt rcvtimeo");
        close(sockfd);
    }

    sprintf(buf, "put %s %d %d", file, index, partSize);
    printf("Sending message: %s\n", buf);

    if ((n = send(sockfd, buf, BUFSIZE, 0)) < 0){
        fprintf(stderr, "Could not send prompt\n");
        close(sockfd);
        return -1;
    }
    bzero(buf, BUFSIZE);
    n = read(sockfd, buf, BUFSIZE);
    if (n <= 0){
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            fprintf(stderr, "Server unavailable could not send chunk\n");
        } else {
            perror("ERROR on read");
        }
        close(sockfd);
        return -1;
    } 
    if (strcmp(buf, "OK") != 0){
        fprintf(stderr, "Internal server error\n");
        close(sockfd);
        return -1;
    } 

    bzero(buf, BUFSIZE);
    int bytesLeft = partSize;
    while (bytesLeft > 0){
        int toRead = (bytesLeft < BUFSIZE) ? bytesLeft : BUFSIZE;
        int bytesRead = fread(buf, 1, toRead, fptr);
        // printf("FILE PART: %s bytes read %d\n", buf, bytesRead);
        if (bytesRead <= 0) {
            // Error reading from file
            fprintf(stderr, "Error reading from file\n");
            close(sockfd);
            return -1;
        }
        n = send(sockfd, buf, bytesRead, 0);
        if (n <= 0) {
            // Error sending data
            fprintf(stderr, "Error sending file\n");
            close(sockfd);
            return -1;
        }
        bytesLeft -= bytesRead;
    }
    printf("Successfully sent chunk\n");
    close(sockfd);
    return 1;
}

void handlePut(struct dfcConf dfc, char **file, int numFile){
    FILE *fptr;
    int fileSize;
    int partSize;
    int remainingPart;
    int n;

    // Loop through each file
    for (int i = 0; i < numFile; i++){
        fptr = fopen(file[i], "rb");
        if (!fptr){
            fprintf(stderr, "Error opening [File] %s cannot send to servers\n", file[i]);
            continue;
        }

        // Get file size
        fseek(fptr, 0, SEEK_END);
        fileSize = ftell(fptr);
        fseek(fptr, 0, SEEK_SET);
        // Split the file into parts
        partSize = fileSize / numServer;
        remainingPart = fileSize % numServer;
        printf("[File] %s size: %d part size: %d\n", file[i], fileSize, partSize);

        // For each part of the file send it to the two servers
        int partSent[numServer];
        memset(partSent, 0, sizeof(partSent));
        
        // Compute md5 and hash of file[i]
        char md5[MD5_DIGEST_LENGTH*2 + 1];
        computeMD5(file[i], md5);
        int x = atoi(md5) % numServer; 

        // Send the file to the first/second set of servers
        for (int set = 0; set < 2; set++){
            fseek(fptr, 0, SEEK_SET);
            // Send the whole file and reset for next set of servers
            for (int partNumber = 0; partNumber < numServer; partNumber++){
                // Find the server to send part of file to
                int toSend1 = (x + partNumber + set) % numServer; // x = HASH(filename) % numServer

                // Connect to the two server file will be sent to
                printf("Sending to server: %d\n", toSend1+1);

                dfc.sockfd[toSend1] = connectToServer(dfc.server[toSend1], dfc.port[toSend1]);
                if (dfc.sockfd[toSend1] < 0){
                    fprintf(stderr, "Could not connect to DFS%d\n", toSend1+1);
                    fseek(fptr, partSize, SEEK_CUR);
                    continue;
                } else {
                    printf("Sending chunk\n");
                    // n = sendChunk(dfc.sockfd[toSend1],file[i], partSize, partNumber+1, fptr);
                    if (partNumber == numServer - 1){
                        n = sendChunk(dfc.sockfd[toSend1], file[i], partSize+remainingPart, partNumber+1, fptr);
                    } else {
                        n = sendChunk(dfc.sockfd[toSend1], file[i], partSize, partNumber+1, fptr);
                    }
                    if (n > 0)
                        partSent[partNumber]+=1;
                }
            }
        }
        for (int j = 0; j < numServer; j++){
            if (partSent[j] == 0){
                fprintf(stderr, "%s put failed\n", file[i]);
                break;
            }
        }
        fclose(fptr);
    }
}

void handleGet(struct dfcConf dfc, char **file, int numFile){
    FILE *fptr;
    bool isRemoved;

    // Loop for each file
    for (int i = 0; i < numFile; i++){
        printf("Getting [File] %s\n", file[i]);
        fptr = fopen(file[i], "wb");
        if (!fptr){
            fprintf(stderr, "Could not create file\n");
            continue;
        }
        isRemoved = false;
        
        // For each chunk 
        for (int j = 1; j < 5; j++){
            // Check each server
            int chunk = -1;
            for (int k = 0; k < numServer; k++){
                // Connect to server
                dfc.sockfd[k] = connectToServer(dfc.server[k], dfc.port[k]);
                if (dfc.sockfd[k] < 0){
                    fprintf(stderr, "Could not connect to DFS%d\n", k+1);
                    continue;
                }
                else    
                    printf("Connecting to server: %d\n", k+1);

                // See if server has chunks
                if ((chunk = getChunk(file[i], j, dfc.sockfd[k], fptr)) > 0)
                    break;
                
            }
            // If chunk is incomplete or doesn't exist delete the file
            if (chunk < 0){
                fclose(fptr);
                remove(file[i]);
                isRemoved = true;
                fprintf(stderr, "%s is incomplete\n", file[i]);
                break;
            }
        }
        // If the file wasn't removed close it
        if (!isRemoved){
            fclose(fptr);
            printf("%s received successfully\n", file[i]);
        }
    }
}

void handleList(struct dfcConf dfc){
    int n;
    char buf[BUFSIZE];

    // Loop through all servers to find files and parts
    for (int server = 0; server < numServer; server++){
        bzero(buf, BUFSIZE);
        strcpy(buf, "list");
        // Connect to server
        dfc.sockfd[server] = connectToServer(dfc.server[server], dfc.port[server]);
        if (dfc.sockfd[server] < 0){
            fprintf(stderr, "Could not connect to DFS%d\n", server+1);
            continue;
        }
        
        // Send list request
        if ((n = send(dfc.sockfd[server], buf, BUFSIZE, 0)) < 0){
            fprintf(stderr, "Could not send prompt\n");
            close(dfc.sockfd[server]);
            continue;;
        }
        bzero(buf, BUFSIZE);
        printf("Server %d files:\n", server+1);
        while ((n = read(dfc.sockfd[server], buf, BUFSIZE)) > 0){
            char fileName[100];
            strcpy(fileName, buf);

            bool isComplete = true;
            // For each file listed check if it can be reconstructed
            for (int j = 1; j < 5; j++){
                // Check each server
                int chunk = -1;
                for (int k = 0; k < numServer; k++){
                    // Connect to server
                    dfc.sockfd[k] = connectToServer(dfc.server[k], dfc.port[k]);
                    if (dfc.sockfd[k] < 0)
                        continue;

                    // See if server has chunks
                    bzero(buf, BUFSIZE);
                    sprintf(buf, "check %s %d", fileName, j);
                    send(dfc.sockfd[k], buf, BUFSIZE, 0);
                    read(dfc.sockfd[k], buf, BUFSIZE);
                    if (strcmp(buf, "confirm") == 0){
                        chunk = 1;
                        break;
                    }
                }
                // If chunk is incomplete or doesn't exist delete the file
                if (chunk < 0){
                    isComplete = false;
                    break;
                }
            }
            if (isComplete){
                bzero(buf, BUFSIZE);
                sprintf(buf, "%s - complete", fileName);
                printf("%s\n", buf);
            } else{
                printf("%s\n", fileName);
            }
            bzero(buf, BUFSIZE);
        }
        if (n < 0){
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("Timeout occurred\n");
            } else {
                perror("ERROR on read");
            }
            continue;
        }
    }
}

int main(int argc, char **argv){
    int sockfd[4], portno1, portno2, portno3, portno4;   // Socket, port to listen, bytesize of client address
    struct dfcConf dfc;

    // Check for valid arguments
    if (argc <= 2 && (strcmp(argv[1], "list") != 0)){
        fprintf(stderr, "%s <command> [file] ... [file]\n", argv[0]);
        exit(1);
    }

    int command = -1;
    if (strcmp(argv[1], "put") == 0)
        command=1;
    if (strcmp(argv[1], "list") == 0)
        command=1;
    if (strcmp(argv[1], "get") == 0)
        command=1;
    if (command == -1){
        fprintf(stderr, "Command must be list/get/put\n");
        exit(1);
    }
    // Initialize the directories for each server
    // For each take the ip address and port number and store it for later use
    FILE *fptr;
    char bufsmall[BUFSMALL];
    char ip[INET_ADDRSTRLEN];
    char serverName[10];
    int port;
    fptr = fopen("dfc.conf", "r");
    if (!fptr){
        fprintf(stderr, "Could not open dfc.conf");
        exit(1);
    } else {
        int i = 0;
        while (fgets(bufsmall, BUFSMALL, fptr)){
            sscanf(bufsmall, "server %s %[^:]:%d", serverName, ip, &port);
            strcpy(dfc.server[i], ip);
            dfc.port[i] = port;
            // printf("%s %d\n", dfc.server[i], dfc.port[i]);
            i++;
            numServer++;
            bzero(bufsmall, BUFSMALL);
        }        
        fclose(fptr);
    }

    // Handle user command
    char **files = argv+2;
    if (strcmp(argv[1], "get") == 0){
        handleGet(dfc, files, argc-2);
    }
    else if (strcmp(argv[1], "put") == 0){
        handlePut(dfc, files, argc-2);
    }
    else if (strcmp(argv[1], "list") == 0){
        handleList(dfc);
    }
    for (int i = 0; i < numServer; i++){
        closeServer(dfc.sockfd[i]);
    }

    return 0;
}