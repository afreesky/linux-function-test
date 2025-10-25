
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define BUFFER_SIZE 512

int copy_file(int dest_fd, int src_fd)
{
    ssize_t bytes_read, bytes_write;
    char buffer[BUFFER_SIZE];

    while ((bytes_read = read(src_fd, buffer, BUFFER_SIZE)) > 0)
    {
        bytes_write = write(dest_fd, buffer, bytes_read);
        if (bytes_write != bytes_read)
        {
            perror("write");
            close(bytes_read);
            close(bytes_write);
            exit(EXIT_FAILURE);
        }
    }

    return 0;
}

pid_t copy_file_with_process(int dest_fd, int src_fd)
{
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (pid == 0)
    {
        printf("new child process!\n");
        copy_file(dest_fd, src_fd);
        exit(EXIT_SUCCESS);
    }
    else
    {
        printf("start child process :%d\n", pid);
        //return back;
    }

    return pid;
}

void blkser_wait_pid(pid_t pid)
{
    pid_t w;
    int wstatus;

    do
    {
        w = waitpid(pid, &wstatus, WUNTRACED | WCONTINUED);
        if (w == -1)
        {
            perror("waitpid");
            exit(EXIT_FAILURE);
        }

        if (WIFEXITED(wstatus))
        {
            printf("exited, status=%d\n", WEXITSTATUS(wstatus));
        }
        else if (WIFSIGNALED(wstatus))
        {
            printf("killed by signal %d\n", WTERMSIG(wstatus));
        }
        else if (WIFSTOPPED(wstatus))
        {
            printf("stopped by signal %d\n", WSTOPSIG(wstatus));
        }
        else if (WIFCONTINUED(wstatus))
        {
            printf("continued\n");
        }
    } while (!WIFEXITED(wstatus) && !WIFSIGNALED(wstatus));
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <bulkdev> <filename> <filename>   -- copy files to bulk\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int dev_fd = open(argv[1], O_RDWR);
    if (dev_fd == -1) {
        perror("open");
        exit(EXIT_FAILURE);
    }

    int fd1 = open(argv[2], O_RDONLY);
    if (dev_fd == -1) {
        perror("open");
        exit(EXIT_FAILURE);
    }

    int fd2 = open(argv[3], O_RDONLY);
    if (dev_fd == -1) {
        perror("open");
        exit(EXIT_FAILURE);
    }

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read, bytes_write;

    bytes_read = read(dev_fd, buffer, BUFFER_SIZE);
    if (bytes_read)
    {
        printf("received client data, start file transfer");
    }

    pid_t pid1 = copy_file_with_process(dev_fd, fd1);
    pid_t pid2 = copy_file_with_process(dev_fd, fd2);

    while ((bytes_read = read(dev_fd, buffer, BUFFER_SIZE)) > 0)
    {
        bytes_write = write(dev_fd, buffer, bytes_read);
        if (bytes_write != bytes_read)
        {
            perror("write");
            break;
        }

        if (bytes_read < BUFFER_SIZE)
            break;
    }

    blkser_wait_pid(pid1);
    blkser_wait_pid(pid2);

    return EXIT_SUCCESS;
}
