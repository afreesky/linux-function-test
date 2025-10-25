
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define BUFFER_SIZE 512

char file_name_path[64];

int main(int argc, char *argv[]) 
{
    int file_fd;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <bulkdev>   -- files to bulk\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int dev_fd = open(argv[1], O_RDWR);
    if (dev_fd == -1) {
        perror("open");
        exit(EXIT_FAILURE);
    }

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read, bytes_write;
    
    while (1)
    {
        memset(buffer, 0, BUFFER_SIZE);

        bytes_read = read(dev_fd, buffer, BUFFER_SIZE);
        if (bytes_read == 0)
            continue;

        memset(file_name_path, 0, 64);
        snprintf(file_name_path, 63, "/mnt/firmware/%s", buffer);

        if (access(file_name_path, R_OK))
        {
            printf("not exit or permission denied : %s\n", file_name_path);
            continue;
        }

        file_fd = open(file_name_path, O_RDONLY);
        if (dev_fd == -1) 
        {
            printf("cannot access %s\n", file_name_path);
            continue;
        }
        
        while ((bytes_read = read(file_fd, buffer, BUFFER_SIZE)) > 0)
        {
            bytes_write = write(dev_fd, buffer, bytes_read);
            if (bytes_write != bytes_read)
            {
                perror("write");
                break;
            }

            if (bytes_read < BUFFER_SIZE)
                break;

            printf("write rawbulk %d\n", bytes_write);
        }

    }

    return EXIT_SUCCESS;
}
