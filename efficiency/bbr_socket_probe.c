/* Local-only TCP data and BBR socket lifecycle probe; no external traffic. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static void fail(const char *what)
{
    perror(what);
    exit(1);
}

static void congestion(int fd, const char *name)
{
    char selected[32] = {0};
    socklen_t size = sizeof(selected);
    if (setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, name, strlen(name)))
        fail("set TCP_CONGESTION");
    if (getsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, selected, &size))
        fail("get TCP_CONGESTION");
    if (strcmp(name, selected)) {
        fprintf(stderr, "congestion mismatch: requested=%s actual=%s\n", name, selected);
        exit(1);
    }
}

static int tcp_socket(void)
{
    struct timeval timeout = {.tv_sec = 5};
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        fail("socket");
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)))
        fail("socket timeout");
    return fd;
}

static unsigned char pattern(unsigned int position, unsigned int seed)
{
    uint32_t n = (position + 1) * UINT32_C(2654435761) ^ seed;
    n ^= n >> 16;
    return (unsigned char)n;
}

static void transfer(int fd, unsigned int seed, int sending)
{
    unsigned char buffer[4096];
    for (unsigned int position = 0; position < 131072; position += sizeof(buffer)) {
        if (position == 65536) {
            congestion(fd, "reno");
            congestion(fd, "bbr");
        }
        if (sending)
            for (unsigned int i = 0; i < sizeof(buffer); i++)
                buffer[i] = pattern(position + i, seed);
        size_t done = 0;
        while (done < sizeof(buffer)) {
            ssize_t size = sending
                ? send(fd, buffer + done, sizeof(buffer) - done, MSG_NOSIGNAL)
                : recv(fd, buffer + done, sizeof(buffer) - done, 0);
            if (size < 0 && errno == EINTR)
                continue;
            if (size <= 0)
                fail(sending ? "send" : "recv");
            done += (size_t)size;
        }
        if (!sending)
            for (unsigned int i = 0; i < sizeof(buffer); i++)
                if (buffer[i] != pattern(position + i, seed)) {
                    fprintf(stderr, "TCP data mismatch at %u\n", position + i);
                    exit(1);
                }
    }
}

int main(void)
{
    char version[8] = {0};
    FILE *file = fopen("/proc/bbr_version", "r");
    if (!file || !fgets(version, sizeof(version), file))
        fail("read /proc/bbr_version");
    fclose(file);
    if (strcmp(version, "3") && strcmp(version, "3\n")) {
        fprintf(stderr, "Expected BBRv3, got %s\n", version);
        return 1;
    }

    for (unsigned int iteration = 0; iteration < 32; iteration++) {
        struct sockaddr_in address = {.sin_family = AF_INET,
                                      .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        socklen_t size = sizeof(address);
        int listener = tcp_socket();
        congestion(listener, "bbr");
        if (bind(listener, (struct sockaddr *)&address, size) || listen(listener, 1) ||
            getsockname(listener, (struct sockaddr *)&address, &size))
            fail("listen");
        pid_t child = fork();
        if (child < 0)
            fail("fork");
        if (!child) {
            close(listener);
            int client = tcp_socket();
            congestion(client, "bbr");
            if (connect(client, (struct sockaddr *)&address, size))
                fail("connect");
            congestion(client, "bbr");
            transfer(client, iteration + 1, 1);
            char ack = 0;
            if (recv(client, &ack, 1, MSG_WAITALL) != 1 || ack != 'Y')
                fail("transfer acknowledgement");
            close(client);
            _exit(0);
        }
        int connection = accept(listener, NULL, NULL);
        if (connection < 0)
            fail("accept");
        close(listener);
        congestion(connection, "bbr");
        transfer(connection, iteration + 1, 0);
        if (send(connection, "Y", 1, MSG_NOSIGNAL) != 1)
            fail("acknowledgement send");
        close(connection);
        int status;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr, "TCP peer failed\n");
            return 1;
        }
    }
    puts("{\"passed\":true,\"bbr_version\":3,\"sessions\":32,\"bytes_verified\":4194304,"
         "\"scope\":\"loopback transfer, passive clone, bbr-reno-bbr switching and socket close\"}");
    return 0;
}
