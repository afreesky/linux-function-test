
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define BUFFER_SIZE 1024

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int fd = open(argv[1], O_RDWR);
    if (fd == -1)
    {
        perror("open");
        exit(EXIT_FAILURE);
    }

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    while ((bytes_read = read(fd, buffer, BUFFER_SIZE)) >= 0)
    {
        if (bytes_read == 0)
        {
            printf("bytes_read 0\n");
            continue;
        }

        if (write(fd, buffer, bytes_read) != bytes_read)
        {
            perror("write");
            close(fd);
            exit(EXIT_FAILURE);
        }
    }

    if (bytes_read == -1)
    {
        perror("read");
        close(fd);
        exit(EXIT_FAILURE);
    }

    close(fd);
    return EXIT_SUCCESS;
}
