
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define BUFFER_SIZE 512

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <bulkdev> <filename>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int dev_fd = open(argv[1], O_RDONLY);
    if (dev_fd == -1) {
        perror("open");
        exit(EXIT_FAILURE);
    }


    int file_fd = open(argv[2], O_CREAT|O_TRUNC|O_RDWR);
    if (file_fd == -1) {
        perror("open");
	    exit(EXIT_FAILURE);
    }

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read, bytes_write;

    while ((bytes_read = read(dev_fd, buffer, BUFFER_SIZE)) > 0) {
	    bytes_write = write(file_fd, buffer, bytes_read);
        if( bytes_write != bytes_read) {
            perror("write");
            close(file_fd);
		    close(dev_fd);
            exit(EXIT_FAILURE);
	}

	if (bytes_read < BUFFER_SIZE)
		break;
    }

    if (bytes_read == -1) {
        perror("read");
    }

    close(dev_fd);
    close(file_fd);

    return EXIT_SUCCESS;
}
