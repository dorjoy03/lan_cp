#define _GNU_SOURCE // needed for struct statx
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <dirent.h>
#include <inttypes.h>
#include <liburing.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <arpa/inet.h>
//#include <netinet/tcp.h>

#include "common.h"

#define QD 128 // queue depth
#define STATX_MASK (STATX_MODE | STATX_SIZE)
#define OPEN_FLAGS O_RDONLY
#define OPEN_MODE 0n

static void usage(const char *progname, FILE *f) {
    fprintf(f, "Usage: %s receiver_ip file/directory...\n", progname);
    fprintf(f, "Send files or directories to machines connected on LAN.\n");
    fprintf(f, "Example: %s 192.168.0.104 file1 file2 dir1 dir2\n",
            progname);
    return;
}

// returns socket descriptor on success, -1 on error
static int setup_socket_and_connect(const char *recv_ip) {
    int ret;
    struct addrinfo hints, *recv_addr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    printf("ip given %s\n", recv_ip);

    ret = getaddrinfo(recv_ip, RCV_PORT, &hints, &recv_addr);
    if (ret != 0) {
        fprintf(stderr, "Failed to get address info for %s: %s\n", recv_ip,
                gai_strerror(ret));
        return -1;
    }

    /* char arr[10000]; */
    /* struct addrinfo *p; */
    /* for (p = rcv_info; p != NULL; p = p->ai_next) { */
    /*     if (p->ai_family == AF_INET) { */
    /*         struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr; */
    /*         inet_ntop(p->ai_family, &(ipv4->sin_addr), arr, sizeof(arr)); */
    /*         printf("ipv4 address: %s\n", arr); */
    /*     } else { */
    /*         struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr; */
    /*         inet_ntop(p->ai_family, &(ipv6->sin6_addr), arr, sizeof(arr)); */
    /*         printf("ipv6 address: %s\n", arr); */
    /*     } */
    /* } */

    int sockfd = socket(recv_addr->ai_family, recv_addr->ai_socktype,
                        recv_addr->ai_protocol);
    if (sockfd == -1) {
        perror("Failed to create socket");
        return -1;
    }

    /* int state = 1; */
    /* setsockopt(sockfd, IPPROTO_TCP, TCP_CORK, &state, sizeof(state)); */
    /* state = 1; */
    /* setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &state, sizeof(state)); */

    if (connect(sockfd, recv_addr->ai_addr, recv_addr->ai_addrlen) != 0) {
        perror("Failed to connect");
        return -1;
    }

    return sockfd;
}

static int send_file(int fd, uint64_t size, int sockfd) {
    size_t lim = 2147479552;
    uint64_t bytes_left = size;
    while (bytes_left) {
        size_t max_count = bytes_left > lim ? lim : (size_t) bytes_left;
        ssize_t sent = sendfile(sockfd, fd, NULL, max_count);
        if (sent < 0) {
            perror("sendfile call failed");
            return -1;
        }
        bytes_left -= (uint64_t) sent;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    /* char cwd[1024]; */
    /* if (getcwd(cwd, sizeof(cwd)) != NULL) { */
    /*     printf("Current working directory: %s\n", cwd); */
    /* } */
    int ret;

    if (argc == 2 && (strcmp(argv[1], "-h") == 0 ||
                      strcmp(argv[1], "--help") == 0)) {
        usage(argv[0], stdout);
        return 0;
    }

    if (argc < 3) {
        usage(argv[0], stderr);
        return 2;
    }

    ret = setup_socket_and_connect(argv[1]);
    if (ret == -1) {
        return 1;
    }
    int sockfd = ret;
    int fds[5];
    uint64_t sizes[5];

    for (int i = 2; i < argc; ++i) {
        struct statx statx_buf;
        ret = statx(AT_FDCWD, argv[i], 0, STATX_MASK, &statx_buf);
        if (ret != 0) {
            fprintf(stderr, "Failed to statx\n");
            return 1;
        }
        ret = open(argv[i], O_RDONLY);
        if (ret < 0) {
            fprintf(stderr, "Failed to open\n");
            return -1;
        }
        fds[i - 2] = ret;
        sizes[i - 2] = statx_buf.stx_size;
    }

    for (int i = 0; i < 2; ++i) {
        ret = send_file(fds[i], sizes[i], sockfd);
        if (ret == -1) {
            fprintf(stderr, "Failed to send file\n");
            return 1;
        }
    }
    close(sockfd);
    return 0;
}
