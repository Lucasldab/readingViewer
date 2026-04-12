/*
 * rv-msg — IPC client for rv (Reading Viewer)
 * Sends commands to a running rv instance via unix socket.
 *
 * Usage: rv-msg <socket-path> <command> [args...]
 *
 * Commands:
 *   open <path>    Append image to bottom of strip
 *   quit           Close viewer
 *   goto <index>   Jump to image N (0-based)
 *   scroll <px>    Scroll by px (negative = up)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: rv-msg <socket-path> <command> [args...]\n");
        fprintf(stderr, "\ncommands:\n");
        fprintf(stderr, "  open <path>    append image\n");
        fprintf(stderr, "  quit           close viewer\n");
        fprintf(stderr, "  goto <index>   jump to image\n");
        fprintf(stderr, "  scroll <px>    scroll by pixels\n");
        return 1;
    }

    const char *sock_path = argv[1];

    /* build command string from remaining args */
    char cmd[4096] = {0};
    for (int i = 2; i < argc; i++) {
        if (i > 2) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("rv-msg: socket");
        return 1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("rv-msg: connect");
        close(fd);
        return 1;
    }

    ssize_t len = strlen(cmd);
    if (write(fd, cmd, len) != len) {
        perror("rv-msg: write");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
