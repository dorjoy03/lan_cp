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

#include "common.h"

#define BUF_SIZE 4194304 // 4MiB

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

    if (connect(sockfd, recv_addr->ai_addr, recv_addr->ai_addrlen) != 0) {
        perror("Failed to connect");
        return -1;
    }

    return sockfd;
}

int main(int argc, char *argv[]) {
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

    size_t len = BUF_SIZE; // 16 pages
    uint8_t buf[BUF_SIZE];
    while (true) {
        send(sockfd, buf, len, 0);
    }

    return 0;
}
