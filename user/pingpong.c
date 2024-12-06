#include "kernel/types.h"
#include "user/user.h"

#define BUFFER_LEN 100
char buffer[BUFFER_LEN];

void echo(int fd);
void send_msg(int fd, char * msg);


int main(int argc, char *argv[])
{
    if (argc != 1) {
        fprintf(2, "Usage: pingpong\n");
        exit(1);
    }

    int ping_pipe[2];
    int status = pipe(ping_pipe);
    if (status != 0) {
        fprintf(2, "open pipe with error code %d\n", status);
        exit(1);
    }

    int pong_pipe[2];
    status = pipe(pong_pipe);
    if (status != 0) {
        fprintf(2, "open pipe with error code %d\n", status);
        exit(1);
    }

    // fork child process
    int pid = fork();
    // parent
    if (pid > 0) {
        // "<pid>: received pong"
        close(ping_pipe[0]);
        send_msg(ping_pipe[1], "ping");
        close(ping_pipe[1]);

        close(pong_pipe[1]);
        echo(pong_pipe[0]);
        close(pong_pipe[0]);
    }
    // child
    else if (pid == 0) {
        // the child should print "<pid>: received ping"
        close(ping_pipe[1]);
        echo(ping_pipe[0]);
        close(ping_pipe[0]);

        close(pong_pipe[0]);
        send_msg(pong_pipe[1], "pong");
        close(pong_pipe[1]);
    }
    // error
    else {
        fprintf(2, "fork error with code %d\n", status);
        exit(1);
    }

    exit(0);
}


void echo(int fd)
{
    int pid = getpid();
    int status = read(fd, buffer, sizeof(buffer));
    if (status < 0) {
        fprintf(2, "read error code %d\n", status);
        exit(1);
    }
    printf("%d: received %s\n", pid, buffer);
}

void send_msg(int fd, char * msg)
{
    int status = write(fd, msg, strlen(msg));
    if (status < 0) {
        fprintf(2, "write error code %d\n", status);
        exit(1);
    }
}