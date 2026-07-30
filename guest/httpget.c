/* httpget — minimal name-aware HTTP fetcher for the LSA guest.
 *
 * Resolves a hostname with musl getaddrinfo() (via /etc/resolv.conf -> slirp's
 * 10.0.2.3 DNS), opens TCP :80, sends an HTTP/1.0 GET, and streams the response
 * body to stdout. This is the out-of-box proof that DNS + TCP work end to end:
 *   lsa httpget example.com
 *   lsa httpget example.com /path
 * Needs the NET_SOCKET capability (granted via /etc/aegis/caps.d/httpget).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: httpget <host> [path]\n");
        return 2;
    }
    const char *host = argv[1];
    const char *path = argc > 2 ? argv[2] : "/";

    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int e = getaddrinfo(host, "80", &hints, &res);
    if (e != 0 || !res) {
        fprintf(stderr, "httpget: cannot resolve %s: %s\n", host, gai_strerror(e));
        return 1;
    }
    char ip[64] = {0};
    struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
    fprintf(stderr, "httpget: %s -> %s\n", host, ip);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("httpget: socket"); freeaddrinfo(res); return 1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("httpget: connect"); freeaddrinfo(res); return 1;
    }
    freeaddrinfo(res);

    char req[512];
    int n = snprintf(req, sizeof req,
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: lsa-httpget\r\n"
        "Connection: close\r\n\r\n", path, host);
    for (int off = 0; off < n; ) {
        ssize_t w = write(fd, req + off, (size_t)(n - off));
        if (w <= 0) { perror("httpget: write"); close(fd); return 1; }
        off += (int)w;
    }

    char buf[2048];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0)
        (void)!write(1, buf, (size_t)r);
    close(fd);
    return 0;
}
