#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <poll.h>

int main(int argc, char **argv) {
    // Create a socket

    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> <sleep>\n", argv[0]);
        exit(0);
    }

    int port = atoi(argv[1]);
    int timeout = atoi(argv[2]);

    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    struct pollfd fds[1];
    fds[0].fd = client_socket;
    fds[0].events = POLLHUP;



    // Specify server address
    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port); // Assuming the server is running on port 80
    server_address.sin_addr.s_addr = inet_addr("127.0.0.1"); // Assuming the server is running locally

    // Connect to the server
    if (connect(client_socket, (struct sockaddr *)&server_address, sizeof(server_address)) == -1) {
        perror("Connection failed");
        exit(EXIT_FAILURE);
    }

    // Send HTTP GET request
    const char *request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";

    if (send(client_socket, request, strlen(request), 0) == -1) 
    {
        perror("Send failed");
        exit(EXIT_FAILURE);
    }

    // Receive response
    char buffer[16024];
    int bytes_received;
    bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);
    printf("Recieved %d bytes of data\n", bytes_received);

    sleep(timeout);

    int ret = poll(fds, 1, 0);
    if (ret == -1) {
        perror("poll");
        // Handle error
    } else if (ret == 0) {
        // No events occurred
    } else {
        // Check for POLLHUP
        if (fds[0].revents & POLLHUP) {
            // Socket closed by remote end
            fprintf(stderr, "Socket closed by remote end\n");
            return 0;
        }
    }

    ssize_t bytes_sent = send(client_socket, request, strlen(request), 0);

    if (bytes_sent == -1) {
        perror("Send failed");
        exit(EXIT_FAILURE);
    } else if (bytes_sent == 0) 
    {
        fprintf(stderr, "Connection closed by remote end\n");
        exit(EXIT_FAILURE);
    }

    bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);

    if (bytes_received == -1) 
    {
        perror("Receive failed");
        exit(EXIT_FAILURE);
    }

    printf("Recieved %d bytes of data\n", bytes_received);

    // Close the socket
    close(client_socket);

    return 0;
}
