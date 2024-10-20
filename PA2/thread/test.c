#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define BUFSIZE 1024

int main(int argc, char **argv) {
    int fileSize = atoi(argv[1]);
    if (fileSize%BUFSIZE != 0)
        fileSize += (BUFSIZE - fileSize%BUFSIZE);

    printf("File size: %d\n", fileSize);
    
    
    return 0;
}