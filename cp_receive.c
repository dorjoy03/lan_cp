#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <inttypes.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "common.h"

static void usage(const char *progname, FILE *f) {
    fprintf(f, "Usage: %s dir\n", progname);
    fprintf(f, "\
Receive files or directories from machines connected on LAN. 'dir' should be\n\
a path to a directory where received contents will be saved. If 'dir' doesn't\n\
exist, we will try to create the specified directory\n");
    fprintf(f, "Example: %s /home/user/Downloads/\n", progname);
    return;
}

int print_my_ip() {
    char host[1024];
    int ret = gethostname(host, 1023);
    if (ret != 0) {
        perror("Failed to get host name");
        return -1;
    }
    host[1023] = '\0';

    struct addrinfo hints, *addr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    ret = getaddrinfo(host, NULL, &hints, &addr);
    if (ret != 0) {
        fprintf(stderr, "Failed to get ip of this machine: %s\n",
                gai_strerror(ret));
        return -1;
    }

    char buf[INET_ADDRSTRLEN];
    //    if (addr->ai_family == AF_INET) { // IPv4
    struct sockaddr_in *ipv4 = (struct sockaddr_in *) addr->ai_addr;
    //    void *res = &(ipv4->sin_addr);
    //    } /* else { // IPv6 */
    /*     struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *) addr->ai_addr; */
    /*     res = &(ipv6->sin6_addr); */
    /* } */

    const char *ip = inet_ntop(addr->ai_family, &(ipv4->sin_addr), buf, sizeof(buf));
    if (ip == NULL) {
        perror("Failed to inet_ntop");
        return -1;
    }

    printf("Use ip address %s to send files or directories\n", ip);
    fflush(stdout);

    freeaddrinfo(addr);
    return 0;
}

void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char *argv[]) {
    int ret;

    if (argc == 2 && (strcmp(argv[1], "-h") == 0 ||
                      strcmp(argv[1], "--help") == 0)) {
        usage(argv[0], stdout);
        return 0;
    }

    if (argc < 2) {
        usage(argv[0], stderr);
        return 2;
    }

    ret = print_my_ip();
    if (ret == -1) {
        return 1;
    }

    struct addrinfo hints, *my_addr;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;     // fill in my IP for me

    ret = getaddrinfo(NULL, RCV_PORT, &hints, &my_addr);
    if (ret != 0) {
        fprintf(stderr, "Failed to get address info of this machine: %s\n",
                gai_strerror(ret));
        return 1;
    }

    int sockfd = socket(my_addr->ai_family, my_addr->ai_socktype,
                        my_addr->ai_protocol);
    if (sockfd == -1) {
        perror("Failed to create socket");
        return 1;
    }

    if (bind(sockfd, my_addr->ai_addr, my_addr->ai_addrlen) == -1) {
        perror("Failed to bind socket");
        return 1;
    }

    if (listen(sockfd, 1) == -1) {
        perror("Failed on listen");
        return 1;
    }

    struct sockaddr_storage sender_addr;
    socklen_t addr_size = sizeof(sender_addr);

    while (true) {
        int sender_fd = accept(sockfd, (struct sockaddr *) &sender_addr,
                               &addr_size);

        if (sender_fd == -1) {
            perror("Failed to accept");
            return 1;
        }

        char buf[INET6_ADDRSTRLEN];
        const char *ip = inet_ntop(sender_addr.ss_family,
                                   get_in_addr((struct sockaddr *) &sender_addr),
                                   buf, sizeof(buf));
        if (ip == NULL) {
            perror("Failed to get sender ip");
            return 1;
        }
        printf("Connected to %s\n", ip);

        uint64_t total = 0;
        while (true) {
            uint8_t arr[1024 * 1024];
            ssize_t cnt = recv(sender_fd, arr, 1024 * 1024, 0);
            if (cnt == 0) {
                printf("got total %lu\n", total);
                break;
            }
            total += (uint64_t) cnt;
        }
    }

    return 0;
}
