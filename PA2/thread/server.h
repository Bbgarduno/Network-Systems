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

#define BUFSIZE 1024
#define INPUTSIZE 200

int sockfd;

void sigint_handler(int sig){
    close(sockfd);
    printf("\nServer exiting successfully\n");
    exit(EXIT_SUCCESS);
} 

void error(char *msg){
  fprintf(stderr, "\033[31mError: \033[0m");
  perror(msg);
  pthread_exit(NULL);
  // // // 
  exit(1);
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
    close(clientfd);
    error("ERROR sending messgage");
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

void *handleClient(void *arg){
  int clientfd = *((int *) arg);
  free(arg);
  char buf[BUFSIZE];
  int n;
  struct timeval tv;
  
  tv.tv_sec = 2;
  tv.tv_usec = 0;
  if (setsockopt(clientfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) < 0){
    close(clientfd);
    perror("ERROR on sockopt");
    return NULL;
  }

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
  char *method, *uri, *version, *save;
  method = strtok_r(buf, " ", &save);
  uri = strtok_r(NULL, " ", &save);
  version = strtok_r(NULL, "\r", &save);

  if (method == NULL || uri == NULL || version == NULL) {
    sendError(clientfd, 400, "Bad Request", "The request could not be parsed or is malformed");
    printf("Thread exiting bad request\n");
    close(clientfd);
    return NULL;
  } else if (strcmp("GET", method) != 0){
    sendError(clientfd, 405, "Method Not Allowed", "A method other than GET was requested");
    printf("Thread exiting method not allowed \n");
    close(clientfd);
    return NULL;
  } else if (strcmp("HTTP/1.0", version) != 0 && strcmp("HTTP/1.1", version) != 0){
    sendError(clientfd, 505, "HTTP Version Not Supported", "An HTTP version other than 1.0 or 1.1 was requested");
    printf("Thread exiting http version not supported\n");
    close(clientfd);
    return NULL;
  }

  /* Protect from overflows */
  method[INPUTSIZE-1] = '\0';
  uri[INPUTSIZE-1] = '\0';
  version[INPUTSIZE-1] = '\0';

  printf("Request method: %s\nRequest URI: %s\nRequest Version: %s\n", method, uri, version);

  /* Open requested file by client */
  FILE *fptr;
  char path[INPUTSIZE+3];
  if (strcmp("/", uri) == 0){ 
    strcpy(path, "www/index.html");
  } else { 
    strcpy(path, "www");
    strcat(path, uri); 
  }
  fptr = fopen(path, "rb");
  
  /* If the file can't be opened figure out the reason */
  if (!fptr){
    printf("Could not open file\n");
    if (errno == EACCES) {        // Check if access is forbidden
        sendError(clientfd, 403, "Forbidden", "The requested file can not be accessed due to a file permission issue");
    } else if (errno == ENOENT) { // Check if file not found
        sendError(clientfd, 404, "Not Found", "The requested file can not be found in the document tree");
    } else {                      // Other error
        sendError(clientfd, 500, "Internal Server Error", "An unexpected error occurred while processing your request");
    }
    printf("Thread exiting could not open file\n");
    close(clientfd);
    return NULL;
  }

  /* Get the size of the file and content type */
  fseek(fptr, 0, SEEK_END);
  int fileSize = ftell(fptr);
  fseek(fptr, 0, SEEK_SET);

  /* Send only in increments of the buffer size if wanted 
  if (fileSize%BUFSIZE != 0)
    fileSize += (BUFSIZE - fileSize%BUFSIZE);
  */
 
  char contentType[25];
  if (fileType(uri, contentType) < 0){
    printf("Error finding content type: %s\n", uri);
    sendError(clientfd, 404, "Not Found", "The requested file type can not be found in the document tree");
    printf("Thread exiting file not found\n");
    close(clientfd);
    return NULL;
  }

  char header[INPUTSIZE];
  int bytesSent = 0;
  snprintf(header, INPUTSIZE, "%s 200 OK\nContent-Type: %s\nContent-Length: %d\r\n\r\n", version, contentType, fileSize);
  bytesSent = sendMsg(clientfd, header, strlen(header));
  if (bytesSent < 0){
    close(clientfd);
    error("ERROR sending message");
  }

  int sent = 0;
  int bytesRead = 0;
  do {
    bzero(buf, sizeof(buf));
    bytesRead = fread(buf, 1, BUFSIZE, fptr);
    if (bytesRead < 0){
      close(clientfd);
      error("ERROR reading from file");
    }
    bytesSent = sendMsg(clientfd, buf, bytesRead);
    if (bytesSent < 0){
      close(clientfd);
      error("ERROR sending message");
    }
    if (bytesSent != bytesRead)
      fprintf(stderr, "Bytes sent != bytes read\n");
    sent+=bytesRead;
  } while (sent < fileSize);

  fclose(fptr);
  // printf("Thread exiting operation completed\n");
  close(clientfd);
  return NULL;
}