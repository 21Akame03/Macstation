// Minimal TCP server for the Pi side — bind, listen, accept, echo.
// No capture/encoding, just proves the socket path works.
//
// Build:  cc -o pi_server pi_server.c
// Run:    ./pi_server            (listens on 0.0.0.0:5000)
//         ./pi_server 5001       (custom port)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(int argc, char *argv[]) {
    int port = (argc > 1) ? atoi(argv[1]) : 5000;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // all interfaces
    addr.sin_port        = htons(port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "bind to port %d failed: %s\n", port, strerror(errno));
        return 1;
    }
    if (listen(sock, 4) != 0) { perror("listen"); return 1; }

    printf("Listening on 0.0.0.0:%d\n", port);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int conn = accept(sock, (struct sockaddr *)&peer, &plen);
        if (conn < 0) { perror("accept"); continue; }

        printf("Connection from %s:%d\n",
               inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

        char buf[1024];
        ssize_t n;
        while ((n = recv(conn, buf, sizeof(buf) - 1, 0)) > 0) {
            buf[n] = '\0';
            printf("  recv (%zd bytes): %s\n", n, buf);
            send(conn, buf, (size_t)n, 0);   // echo it back
        }
        printf("Connection closed\n");
        close(conn);
    }
    // unreachable
}
