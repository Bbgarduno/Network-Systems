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
#include <openssl/md5.h>
#include <sys/stat.h>

#define BUFSIZE 1024
#define INPUTSIZE 200

pthread_t timerThread;
int sockfd;
pthread_mutex_t fileLock;
pthread_mutex_t blockLock;
int cacheTime = 30;

void sigint_handler(int sig){
    close(sockfd);
    pthread_cancel(timerThread);
    printf("\nServer exiting successfully\n");
    exit(EXIT_SUCCESS);
} 

void error(char *msg){
  fprintf(stderr, "\033[31mError: \033[0m");
  perror(msg);
  // pthread_exit(NULL);
}

void sendError(int clientfd, int errorNum, char *reason, char *msg){
  char response[INPUTSIZE];
  snprintf(response, INPUTSIZE, "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n%s", errorNum, reason, strlen(msg), msg);
  if (send(clientfd, response, INPUTSIZE, 0) < 0){
    close(clientfd);
    error("ERROR sending error");
  }
}

int sendMsg(int clientfd, char *msg, int length){
  int bytes;
  if ((bytes = send(clientfd, msg, length, 0)) < 0){
    error("ERROR sending messgage");
    return -1;
  }
  return bytes;
}

int fileType(char *uri, char *contentType){
  if (strcmp(uri, "/") == 0){
    strcpy(contentType, "text/html");
    return 0;
  }

  char *extension = strrchr(uri, '.');
  if (extension){
    if (strcmp(extension, ".html") == 0)
      strcpy(contentType, "text/html");
    else if (strcmp(extension, ".txt") == 0)
      strcpy(contentType, "text/plain");
    else if (strcmp(extension, ".png") == 0)
      strcpy(contentType, "image/png");
    else if (strcmp(extension, ".gif") == 0)
      strcpy(contentType, "image/gif");
    else if (strcmp(extension, ".jpg") == 0)
      strcpy(contentType, "image/jpg");
    else if (strcmp(extension, ".ico") == 0)
      strcpy(contentType, "image/x-icon");
    else if (strcmp(extension, ".css") == 0)
      strcpy(contentType, "text/css");
    else if (strcmp(extension, ".js") == 0)
      strcpy(contentType, "application/javascript");
    else
      return -1;
    return 0;
  }
  return -1;
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

int checkBlocklist(char *hostname, char *ip){
  FILE *fptr;
  char blocked[200];
  pthread_mutex_lock(&blockLock);
  fptr = fopen("./blocklist", "rb");

  if (!fptr){
    fptr = fopen("blocklist", "w");
    fclose(fptr);
    pthread_mutex_unlock(&blockLock);
    return 1;
  }

  while (fgets(blocked, sizeof(blocked), fptr)){
    if (strcmp(blocked, hostname) == 0 || strcmp(blocked, ip) == 0)
      fclose(fptr);
      pthread_mutex_unlock(&blockLock);
      return -1;
  }

  pthread_mutex_unlock(&blockLock);
  fclose(fptr);
  return 1;
}

void *handleClient(void *arg){
  // struct args argument = *((struct args *) arg);
  int clientfd = *((int *) arg);
  free(arg);
  char buf[BUFSIZE];
  int n;
  struct timeval tv;
  FILE *fptr;
  
  tv.tv_sec = 4;
  tv.tv_usec = 0;
  if (setsockopt(clientfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) < 0){
    close(clientfd);
    perror("ERROR on sockopt");
    return NULL;
  }

  bzero(buf, BUFSIZE);

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
  
  /* Scan message for error in request */
  char *method, *uri, *version, *save, *link;
  method = strtok_r(buf, " ", &save);
  uri = strtok_r(NULL, " ", &save);
  version = strtok_r(NULL, "\r\n", &save);
  // hostname = strtok_r(NULL, "\r", &save);
  // hostname = strchr(hostname, ' ') + 1;
  link = uri;
  uri = strchr(uri + strlen("http://"), '/');
  char versionSave[INPUTSIZE];
  char uriSave[INPUTSIZE];
  strcpy(versionSave, version);
  strcpy(uriSave, uri);

  if (method == NULL || uri == NULL || version == NULL) {
    sendError(clientfd, 400, "Bad Request", "The request could not be parsed or is malformed");
    printf("Thread exiting bad request\n");
    close(clientfd);
    return NULL;
  } else if (strcmp("GET", method) != 0){
    sendError(clientfd, 400, "Bad Request", "A method other than GET was requested");
    printf("Thread exiting method not allowed \n");
    close(clientfd);
    return NULL;
  } else if (strcmp("HTTP/1.0", version) != 0 && strcmp("HTTP/1.1", version) != 0){
    sendError(clientfd, 505, "HTTP Version Not Supported", "An HTTP version other than 1.0 or 1.1 was requested");
    printf("Thread exiting http version not supported\n");
    close(clientfd);
    return NULL;
  }

  char *hostStart = strstr(link, "://") + 3;
  char *hostEnd = strchr(hostStart, '/');
  int hostnameLength = hostEnd - hostStart;
  char hostname[hostnameLength + 1];
  strncpy(hostname, hostStart, hostnameLength);
  hostname[hostnameLength] = '\0';
  
  printf("Method: %s\nURI: %s\nVersion: %s\nHostname: %s\nLink: %s\n", method, uri, version, hostname, link);

  int servSockfd;
  struct sockaddr_in serv_addr; 
  struct hostent *server;

  server = gethostbyname(hostname);
  if (!server){
    sendError(clientfd, 400, "Could not resolve hostname", "Website cannot be found");
    error("ERROR in gethostbyname");
    close(clientfd);
    return NULL;
  }

  char ip[24];
  if (inet_ntop(AF_INET, (char *)server->h_addr, ip, sizeof(ip)) < 0){
    sendError(clientfd, 500, "Internal Server Error", "An unexpected error occurred while processing your request");
    error("ERROR in inet_ntop");
    close(clientfd);
    return NULL;
  }

  if ((n = checkBlocklist(hostname, ip)) < 0){
    if (n == -1){
      sendError(clientfd, 403, "Forbidden", "Cannot access host");
      printf("Client request blocked\n");
    } else {
      sendError(clientfd, 500, "Internal Server Error", "An unexpected error occurred while processing your request");
      error("Could not create/open blocklist");
    }
    close(clientfd);
    return NULL;
  }

  // Caching
  char md5[MD5_DIGEST_LENGTH*2 + 1];
  computeMD5(link, md5);
  printf("Hash: %s\n", md5);
  char path[strlen(md5) + strlen("cache/") + 1];
  strcpy(path, "cache/");
  strcat(path, md5);

  pthread_mutex_lock(&fileLock);
  fptr = fopen(path, "rb");
  if (fptr){
    int bytesRead = 0;
    bzero(buf, BUFSIZE);
    char contentType[25];

    fseek(fptr, 0, SEEK_END);
    int fileSize = ftell(fptr);
    fseek(fptr, 0, SEEK_SET);

    if (fileType(uriSave, contentType) < 0){
      printf("Error finding content type: %s\n", uriSave);
      fclose(fptr);
      pthread_mutex_unlock(&fileLock);
      sendError(clientfd, 404, "Not Found", "The requested file type can not be found in the document tree");
      printf("Thread exiting file not found\n");
      close(clientfd);
      return NULL;
    }
    char header[INPUTSIZE];
    snprintf(header, INPUTSIZE, "%s 200 OK\nContent-Type: %s\nContent-Length: %d\r\n\r\n", versionSave, contentType, fileSize);
    if (sendMsg(clientfd, header, strlen(header)) < 0){
      fclose(fptr);
      pthread_mutex_unlock(&fileLock);
      close(clientfd);
      error("ERROR sending message");
      return NULL;
    }
    printf("HEADER: %s\n", header);

    while ((bytesRead = fread(buf, 1, BUFSIZE, fptr)) > 0){
      // printf("%s", buf);
      if (sendMsg(clientfd, buf, bytesRead) < 0){
        fclose(fptr);
        pthread_mutex_unlock(&fileLock);
        error("ERROR sending HTTP request");
        close(clientfd);
        return NULL;
      }  
      bzero(buf, BUFSIZE);
    }

    fclose(fptr);
    pthread_mutex_unlock(&fileLock);
    close(clientfd);
    printf("Sent from cache. Thread exiting operation completed\n");
    return NULL;
  } else {
    pthread_mutex_unlock(&fileLock);
  }

  // open a new socket to communicate 
  servSockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (servSockfd < 0){
    error("ERROR opening socket");
    close(clientfd);
    close(servSockfd);
    return NULL;      
  }

  bzero((char *) &serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(80); // HTTP port
  bcopy((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr, server->h_length);

  // Connect to the requested server
  if (connect(servSockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0){
    error("ERROR using connect");
    close(clientfd);
    close(servSockfd);
    return NULL;
  }

  int requestSize = strlen(uri) + strlen(version) + hostnameLength + 20;
  char *request = malloc(requestSize);
  sprintf(request, "GET %s %s\r\nHost: %s\r\n\r\n", uri, version, hostname);
  printf("Connected to: %s\n", hostname);
  printf("HTTP Request >\n%s\n", request);

  if (sendMsg(servSockfd, request, requestSize) < 0){
    free(request);
    error("ERROR sending HTTP request");
    close(clientfd);
    close(servSockfd);
    return NULL;
  }
  free(request);

  /* Create requested file by client */
  // Prevent race conditions -- used mutex locks
  pthread_mutex_lock(&fileLock);
  fptr = fopen(path, "wb");
  if (!fptr){
    pthread_mutex_unlock(&fileLock);
    sendError(clientfd, 500, "Internal Server Error", "An unexpected error occurred while processing your request");
    error("Could not create/open file");
    close(clientfd);
    close(servSockfd);
    return NULL;
  }

  int bytesRead = 0;
  int bytesSent = 0;
  bool headerComplete = false;
  bzero(buf, sizeof(buf));

  while ((bytesRead = read(servSockfd, buf, sizeof(buf))) > 0){
    if (!headerComplete) {
        char *bodyStart = strstr(buf, "\r\n\r\n");
        if (bodyStart) {
            // Found the end of headers, move pointer to start of body
            bodyStart += 4;
            int bodyLength = bytesRead - (bodyStart - buf);
            fwrite(bodyStart, 1, bodyLength, fptr); // Write only the body to cache
            headerComplete = true;
        }
    } else {
        // Write the entire content to the cache
        fwrite(buf, 1, bytesRead, fptr);
    }

    if ((bytesSent = sendMsg(clientfd, buf, bytesRead) < 0)){
      fclose(fptr);
      pthread_mutex_unlock(&fileLock);
      error("ERROR sending HTTP request");
      close(clientfd);
      close(servSockfd);
      return NULL;
    }  
    bzero(buf, sizeof(buf));
  }

  fclose(fptr);
  pthread_mutex_unlock(&fileLock);
  close(servSockfd);
  close(clientfd);
  printf("Thread exiting operation completed\n");
  return NULL;
}

void *cleanCache(){
  while (1) {
    DIR *dir = opendir("cache/");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {  // Regular file
                char filePath[INPUTSIZE];
                snprintf(filePath, sizeof(filePath), "%s%s", "cache/", entry->d_name);

                struct stat fileStat;
                if (stat(filePath, &fileStat) == 0) {
                    time_t currentTime = time(NULL);
                    time_t fileModificationTime = fileStat.st_mtime;
                    if ((currentTime - fileModificationTime) > cacheTime) {
                        // File has expired, remove it
                        pthread_mutex_lock(&fileLock);
                        remove(filePath);
                        pthread_mutex_unlock(&fileLock);
                        printf("Expired file removed: %s\n", entry->d_name);
                    }
                }
            }
        }
        closedir(dir);
    } else {
      mkdir("./cache", 0777);
    }
    sleep(1); 
  }
  return NULL;
}