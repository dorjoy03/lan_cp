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

#define QD 4096

enum op_type {
    SOCKET_IO,
    FILE_IO,
};

struct type_data {
    enum op_type type;
    void *buf;
    size_t len;
    uint64_t offset;
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

    char buffer[INET_ADDRSTRLEN];
    //    if (addr->ai_family == AF_INET) { // IPv4
    struct sockaddr_in *ipv4 = (struct sockaddr_in *) addr->ai_addr;
    //    void *res = &(ipv4->sin_addr);
        //    } /* else { // IPv6 */
    /*     struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *) addr->ai_addr; */
    /*     res = &(ipv6->sin6_addr); */
    /* } */

    const char *ip = inet_ntop(addr->ai_family, &(ipv4->sin_addr), buffer, sizeof(buffer));
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

    /* int pipes[2]; */
    /* ret = pipe(pipes); */

    /* if (ret != 0) { */
    /*     perror("Failed to create pipe"); */
    /*     return 1; */
    /* } */

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

    char buffer[INET6_ADDRSTRLEN];
    const char *ip = inet_ntop(sender_addr.ss_family,
                               get_in_addr((struct sockaddr *) &sender_addr),
                               buffer, sizeof(buffer));
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

    size_t len = 65536;
    void *buf = malloc(len * QD);
    if (buf == NULL) {
        fprintf(stderr, "Failed to allocate memory\n");
        return 1;
    }

    void *free_bufs[QD];
    for (int i = 0; i < QD; ++i) {
        free_bufs[i] = buf + (size_t) i * len;
    }

    int free_bufs_ind = 0;

    struct type_data datas[QD];
    struct type_data *free_datas[QD];
    for (int i = 0; i < QD; ++i) {
        free_datas[i] = &datas[i];
    }
    int free_datas_ind = 0;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    assert(sqe);

    struct type_data *tmp = free_datas[free_datas_ind++];
    tmp->buf = free_bufs[free_bufs_ind++];
    tmp->len = len;
    tmp->type = SOCKET_IO;

    io_uring_prep_recv(sqe, sender_fd, tmp->buf, len, 0);
    io_uring_sqe_set_data(sqe, tmp);
    ret = io_uring_submit(&ring);

    if (ret < 0) {
        fprintf(stderr, "Failed to submit to ring: %s\n",
                strerror(-ret));
        return 1;
    }

    uint64_t cur_off = 0;
    //    int cnt = 0;

    while (true) {
        bool cqe_seen = false;
        bool submit = false;
        //  ++cnt;
        //        printf("here %d\n", cnt);
        struct io_uring_cqe *cqe;
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "Failed to get completion: %s\n",
                    strerror(-ret));
            return 1;
        }

        struct type_data *data = io_uring_cqe_get_data(cqe);

        if (cqe->res < 0) { // failed
            if (cqe->res == -EAGAIN) {
                submit = true;
                if (data->type == SOCKET_IO) {
                    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                    assert(sqe);
                    io_uring_prep_recv(sqe, sender_fd, data->buf, len, 0);
                    io_uring_sqe_set_data(sqe, data);
                } else {
                    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                    assert(sqe);
                    io_uring_prep_write(sqe, fd, data->buf, (unsigned) data->len,
                                        data->offset);
                    io_uring_sqe_set_data(sqe, data);
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
            if (data->type == SOCKET_IO) {
                //                printf("here at socket_io\n");
                if (free_datas_ind == QD) {
                    //printf("here at free_datas_ind check\n");
                    io_uring_cqe_seen(&ring, cqe);
                    cqe_seen = true;
                    struct io_uring_cqe *cqein;
                    ret = io_uring_wait_cqe(&ring, &cqein);
                    if (ret < 0) {
                        fprintf(stderr, "Failed to get completion: %s\n",
                                strerror(-ret));
                        return 1;
                    }
                    struct type_data *ptr = io_uring_cqe_get_data(cqein);
                    //printf("type = %d\n", ptr->type);
                    assert(ptr->type == FILE_IO);
                    free_datas[--free_datas_ind] = ptr;
                    free_bufs[--free_bufs_ind] = ptr->buf;
                    io_uring_cqe_seen(&ring, cqein);
                }
                //printf("here at sqe\n");
                struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                assert(sqe);
                struct type_data *tmp = free_datas[free_datas_ind++];
                tmp->buf = data->buf;
                tmp->len = (size_t) cqe->res;
                tmp->type = FILE_IO;
                tmp->offset = cur_off;
                cur_off += (uint64_t) cqe->res;
                io_uring_prep_write(sqe, fd, tmp->buf, (unsigned) tmp->len, tmp->offset);
                io_uring_sqe_set_data(sqe, tmp);

                data->buf = free_bufs[free_bufs_ind++];

                struct io_uring_sqe *sqe2 = io_uring_get_sqe(&ring);
                assert(sqe2);
                io_uring_prep_recv(sqe2, sender_fd, data->buf, len, 0);
                io_uring_sqe_set_data(sqe2, data);
                submit = true;
            } else {
                //printf("here at free\n");
                free_datas[--free_datas_ind] = data;
                free_bufs[--free_bufs_ind] = data->buf;
            }
        }

        if (!cqe_seen) {
            io_uring_cqe_seen(&ring, cqe);
        }
        //printf("here at sbumit\n");
        if (submit) {
            ret = io_uring_submit(&ring);
            if (ret < 0) {
                fprintf(stderr, "Failed to submit to ring: %s\n",
                        strerror(-ret));
                return 1;
            }
        }
    }

    return 0;
}
