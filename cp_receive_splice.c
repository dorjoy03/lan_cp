#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <inttypes.h>
#include <liburing.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "common.h"

#define QD 128

enum op_type {
    SOCKET_TO_PIPE,
    PIPE_TO_FILE,
};

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

    int pipes[2];
    ret = pipe(pipes);

    if (ret != 0) {
        perror("Failed to create pipe");
        return 1;
    }

    /* int check = fcntl(pipes[0], F_GETPIPE_SZ); */
    /* printf("0 size %d\n", check); */
    /* check = fcntl(pipes[1], F_GETPIPE_SZ); */
    /* printf("1 size %d\n", check); */
    /* check = fcntl(pipes[0], F_SETPIPE_SZ, 1048576); */
    /* printf("0 set size %d\n", check); */
    /* check = fcntl(pipes[0], F_GETPIPE_SZ); */
    /* printf("0 size %d\n", check); */
    /* check = fcntl(pipes[1], F_GETPIPE_SZ); */
    /* printf("1 size %d\n", check); */
    /* check = fcntl(pipes[1], F_SETPIPE_SZ, 1048576); */
    /* printf("1 set size %d\n", check); */

    struct sockaddr_storage sender_addr;
    socklen_t addr_size = sizeof(sender_addr);

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

    int fd = open("dir/test", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("Failed to open dir/test");
        return 1;
    }

    /* uint64_t total = 0; */
    /* while (true) { */
    /*     //uint8_t arr[1024 * 1024]; */
    /*     //ssize_t cnt = recv(sender_fd, arr, 1024 * 1024, 0); */
    /*     ssize_t cnt = splice(sender_fd, NULL, pipes[1], NULL, 1024 * 1024, 0); */
    /*     if (cnt == 0) { */
    /*         printf("got total %lu\n", total); */
    /*         break; */
    /*     } */
    /*     ssize_t cnt2 = splice(pipes[0], NULL, fd, NULL, 1024 * 1024, 0); */
    /*     total += (uint64_t) cnt2; */
    /* } */

    struct io_uring ring;
    ret = io_uring_queue_init(QD, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to set up io_uring: %s\n", strerror(-ret));
        return 1;
    }

    unsigned int size = 1024 * 1024;

    enum op_type socket_to_pipe = SOCKET_TO_PIPE;
    enum op_type pipe_to_file = PIPE_TO_FILE;

    struct io_uring_sqe *sqe1 = io_uring_get_sqe(&ring);
    assert(sqe1);
    io_uring_prep_splice(sqe1, sender_fd, -1, pipes[1], -1, size, 0);
    io_uring_sqe_set_data(sqe1, &socket_to_pipe);

    struct io_uring_sqe *sqe2 = io_uring_get_sqe(&ring);
    assert(sqe2);
    io_uring_prep_splice(sqe2, pipes[0], -1, fd, -1, size, 0);
    io_uring_sqe_set_data(sqe2, &pipe_to_file);

    ret = io_uring_submit(&ring);
    if (ret < 0) {
        fprintf(stderr, "Failed to submit to ring: %s\n",
                strerror(-ret));
        return 1;
    }

    while (true) {
        struct io_uring_cqe *cqe;
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "Failed to get completion: %s\n",
                    strerror(-ret));
            return 1;
        }

        enum op_type *data = io_uring_cqe_get_data(cqe);

        if (cqe->res < 0) { // failed
            if (cqe->res == -EAGAIN) {
                if (*data == socket_to_pipe) {
                    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                    assert(sqe);
                    io_uring_prep_splice(sqe, sender_fd, -1, pipes[1], -1, size, 0);
                    io_uring_sqe_set_data(sqe, &socket_to_pipe);
                } else {
                    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                    assert(sqe);
                    io_uring_prep_splice(sqe, pipes[0], -1, fd, -1, size, 0);
                    io_uring_sqe_set_data(sqe, &pipe_to_file);
                }
            } else {
                fprintf(stderr, "failed operaion in io: %s\n",
                        strerror(-cqe->res));
                return 1;
            }
        } else { // succeeded
            if (cqe->res == 0) {
                fprintf(stdout, "got 0\n");
                return 1;
            }
            if (*data == socket_to_pipe) {
                struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                assert(sqe);
                io_uring_prep_splice(sqe, sender_fd, -1, pipes[1], -1, size, 0);
                io_uring_sqe_set_data(sqe, &socket_to_pipe);
            } else {
                struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                assert(sqe);
                io_uring_prep_splice(sqe, pipes[0], -1, fd, -1, size, 0);
                io_uring_sqe_set_data(sqe, &pipe_to_file);
            }
        }

        io_uring_cqe_seen(&ring, cqe);

        ret = io_uring_submit(&ring);
        if (ret < 0) {
            fprintf(stderr, "Failed to submit to ring: %s\n",
                    strerror(-ret));
            return 1;
        }
    }

    return 0;
}
