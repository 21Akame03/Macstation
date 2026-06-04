// Minimal TCP client for the host (Mac) side — connect, send, recv echo.
// No capture/encoding, just proves the socket path works.
//
// Build:  cc -o host_client host_client.c
// Run:    ./host_client                       (connects to 192.168.7.1:5000)
//         ./host_client 192.168.7.1 5000       (explicit ip/port)
//
// NOTE: run this from your OWN terminal (iTerm/Terminal). macOS Local Network
// Privacy can block a freshly-built binary on the USB-gadget link; the first run
// should prompt "wants to find devices on your local network" — click Allow.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(int argc, char *argv[]) {
    const char *ip = (argc > 1) ? argv[1] : "192.168.7.1";
    int port       = (argc > 2) ? atoi(argv[2]) : 5000;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad address: %s\n", ip);
        return 1;
    }

    printf("Connecting to %s:%d ...\n", ip, port);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "connect failed: %s\n", strerror(errno));
        return 1;
    }
    printf("Connected.\n");

    const char *msg = "hello from host";
    if (send(sock, msg, strlen(msg), 0) < 0) { perror("send"); return 1; }
    printf("Sent: %s\n", msg);

    char buf[1024];
    ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
        printf("Echo (%zd bytes): %s\n", n, buf);
        printf("SUCCESS: socket round-trip works.\n");
    } else if (n == 0) {
        printf("Server closed connection without echo.\n");
    } else {
        perror("recv");
    }

    close(sock);
    return 0;
}
