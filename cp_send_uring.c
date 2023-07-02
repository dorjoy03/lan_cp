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

#define QD 128 // queue depth
#define STATX_MASK (STATX_MODE | STATX_SIZE)
#define OPEN_FLAGS O_RDONLY
#define BUFSIZE 4194304 // 4 MiB - max value of command "sysctl net.ipv4.tcp_wmem"

struct io_data {
    void *buf;
    off_t offset;
};

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

    struct io_uring ring;
    ret = io_uring_queue_init(QD, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to set up io_uring: %s\n", strerror(-ret));
        goto err2;
    }

    int fd = open(argv[2], OPEN_FLAGS);
    if (fd < 0) {
        perror("Failed to open file");
        return 1;
    }
    struct statx statx_buf;
    ret = statx(AT_FDCWD, argv[2], 0, STATX_MASK, &statx_buf);
    if (ret != 0) {
        fprintf(stderr, "failed to statx");
        return 1;
    }
    uint64_t size = statx_buf.stx_size;
    uint64_t cur_offset = 0;
    uint64_t left = size;

    uint8_t *buffer = (uint8_t *) malloc(QD * BUFSIZE);
    if (buffer == NULL) {
        perror("Failed to malloc\n");
        return 1;
    }

    void *free_buf_ptrs[QD];
    for (int i = 0; i < QD; ++i) {
        free_buf_ptrs[i] = buffer + (i * BUFSIZE);
    }
    int free_ind = 0;

    struct io_data datas[QD];
    int free_data_ind = 0;

    struct io_data *free_data_ptrs[QD];
    for (int i = 0; i < QD; ++i) {
        free_data_ptrs[i] = &datas[i];
    }

    int wait_cnt = 0;
    for (; left > 0 && free_ind < QD; ++free_ind) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        assert(sqe);
        unsigned bufsize = left < BUFSIZE ? left : BUFSIZE;
        io_uring_prep_read(sqe, fd, free_buf_ptrs[free_ind], bufsize, cur_offset);
        struct io_data *dat = free_data_ptrs[free_data_ind++];
        dat->buf = free_buf_ptrs[free_ind];
        dat->offset = cur_offset;
        cur_offset += bufsize;
        left -= bufsize;
        io_uring_sqe_set_data(sqe, dat);
        ++wait_cnt;
    }

    if (wait_cnt) {
        ret = io_uring_submit(&ring);
        if (ret < 0) {
            fprintf(stderr, "Failed to submit to ring: %s\n",
                    strerror(-ret));
            goto err;
        }
    }

    uint64_t total_sent = 0;

    while (true) {
        void *bufs_to_send[QD];

        int lim = QD;
        if (wait_cnt) {
            int small = lim < wait_cnt ? lim : wait_cnt;
            for (int i = 0; i < small; ++i) {
                struct io_uring_cqe *cqe;
                ret = io_uring_wait_cqe(&ring, &cqe);
                if (ret != 0) {
                    fprintf(stderr, "failed to wait cqe\n");
                    return 1;
                }
                struct io_data *ptr = io_uring_cqe_get_data(cqe);
                bufs_to_send[i] = ptr->buf;
                free_data_ptrs[--free_data_ind]= ptr;
                free_buf_ptrs[--free_ind] = ptr->buf;
                io_uring_cqe_seen(&ring, cqe);
            }

            for (int i = 0; i < small; ++i) {
                total_sent += BUFSIZE;
                send(sockfd, bufs_to_send[i], BUFSIZE, 0);
            }
            wait_cnt -= small;
        }

        bool should_submit = false;

        for (; left > 0 && free_ind < QD; ++free_ind) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            assert(sqe);
            unsigned bufsize = left < BUFSIZE ? left : BUFSIZE;
            io_uring_prep_read(sqe, fd, free_buf_ptrs[free_ind],
                               bufsize, cur_offset);
            struct io_data *dat = free_data_ptrs[free_data_ind++];
            dat->buf = free_buf_ptrs[free_ind];
            dat->offset = cur_offset;
            cur_offset += bufsize;
            left -= bufsize;
            io_uring_sqe_set_data(sqe, dat);
            should_submit = true;
            ++wait_cnt;
        }

        if (should_submit) {
            ret = io_uring_submit(&ring);
            if (ret < 0) {
                fprintf(stderr, "Failed to submit to ring: %s\n",
                        strerror(-ret));
                goto err;
            }
        }

        if (!wait_cnt) {
            break;
        }
        //printf("left %lld\n", left);
    }

    printf("total sent %lld\n", total_sent);

    io_uring_queue_exit(&ring);
    close(sockfd);
    return 0;

err:
    io_uring_queue_exit(&ring);
err2:
    close(sockfd);
    return 1;
}
