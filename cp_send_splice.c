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
#define QDT 126
#define PAGE 65536
#define STATX_MASK (STATX_MODE | STATX_SIZE)
#define OPEN_FLAGS O_RDONLY
#define OPEN_MODE 0n

int pipes[2];

enum op_type {
    STATX,
    OPEN,
    CLOSE,
    FILE_TO_PIPE,
    PIPE_TO_SOCKET
};

struct op_data {
    enum op_type type;
    char *argv_ptr;
    char *path;
    size_t len;
    int fd; // file descriptor for closing
    uint64_t to_splice;
    uint64_t spliced;
    struct statx statx_buf;
};

struct splice_data {
    int fd;
    uint64_t to_splice;
};

struct loop_info {
    int to_submit;
    int to_wait;
    int argc_ind;
    int cur_free_ind; // current free index in op_data_ptrs
    struct op_data *op_data_ptrs[QD];
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

static int prep_statx_data(struct op_data *data, char *path) {
    size_t len = strlen(path);
    char *str = NULL;
    if (data->len >= len + 1) {
        str = data->path;
    } else {
        str = (char *) realloc(str, len + 1);
        if (str == NULL) {
            perror("Failed to allocate memory");
            return -1;
        }
        data->len = len + 1;
    }
    char *dst = strcpy(str, path);
    data->type = STATX;
    data->path = dst;
    //data->argv_ptr = argv_path;
    return 0;
}

static void prep_op(struct io_uring *ring, struct op_data *data,
                    struct loop_info *loop_data) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    assert(sqe);

    switch (data->type) {
    case STATX:
        io_uring_prep_statx(sqe, AT_FDCWD, data->path, 0, STATX_MASK,
                            &(data->statx_buf));
        break;
    case OPEN:
        io_uring_prep_openat(sqe, AT_FDCWD, data->path, OPEN_FLAGS, 0);
        break;
    case FILE_TO_PIPE:
        io_uring_prep_splice(sqe, data->fd, -1, pipes[1], -1, PAGE, 0);
        break;
    case PIPE_TO_SOCKET:
        io_uring_prep_splice(sqe, pipes[0], -1, data->fd, -1, PAGE, 0);
        break;
    case CLOSE:
        io_uring_prep_close(sqe, data->fd);
        break;
    default:
        fprintf(stderr, "Unexpected op type\n");
        exit(1);
    }

    io_uring_sqe_set_data(sqe, data);
    loop_data->to_submit++;
    return;
}

/* static int send_file(struct op_data *data, int fd, int sockfd) { */
/*     uint64_t bytes_left = data->statx_buf.stx_size; */
/*     while (bytes_left) { */
/*         size_t max_count = bytes_left > SIZE_MAX */
/*             ? SIZE_MAX */
/*             : (size_t) bytes_left; */
/*         ssize_t sent = sendfile(sockfd, fd, NULL, max_count); */
/*         if (sent < 0) { */
/*             perror("sendfile call failed"); */
/*             return -1; */
/*         } */
/*         printf("initial bytes left %lu\n", bytes_left); */
/*         bytes_left -= (uint64_t) sent; */
/*         printf("bytes left %lu\n", bytes_left); */
/*     } */

/*     return 0; */
/* } */

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

    ret = pipe(pipes);
    if (ret != 0) {
        fprintf(stderr, "Failed to create pipe: %s\n", strerror(ret));
        return 1;
    }

    struct io_uring ring;
    ret = io_uring_queue_init(QD, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to set up io_uring: %s\n", strerror(-ret));
        goto err2;
    }

    struct op_data op_datas[QD];
    /* struct op_data *free_op_data_ptrs[QD]; */
    /* int free_op_data_ind = 0; */
    /* for (int i = 0; i < QD; ++i) { */
    /*     op_datas[i].path = NULL; */
    /*     op_datas[i].len = 0; */
    /*     free_op_data_ptrs[i] = &(op_datas[i]); */
    /* } */

    struct loop_info loop_data;
    loop_data.argc_ind = 2;
    loop_data.to_submit = 0;
    loop_data.to_wait = 0;
    loop_data.cur_free_ind = 0;

    for (int i = 0; i < QD; ++i) {
        op_datas[i].path = NULL;
        op_datas[i].len = 0;
        loop_data.op_data_ptrs[i] = &(op_datas[i]);
    }

    struct splice_data fds_to_splice[QD];
    int fds_to_splice_ind = -1;
    int next_splice_fd_ind = 0;
    bool file_to_pipe_ongoing = false;
    bool splice_wait = false;
    bool submitted_pipe = false;

    uint64_t piped = 0;
    uint64_t socketed = 0;

    while (true) {
        /* printf("to submit = %d\nto_wait = %d\nargc_ind %d\n", */
        /*        loop_data.to_submit, loop_data.to_wait, argc - loop_data.argc_ind); */
        if (loop_data.to_submit == 0 && (loop_data.to_wait == 0 || (submitted_pipe && loop_data.to_wait == 1)) &&
            loop_data.argc_ind == argc) {
            /* printf("to submit = %d\nto_wait = %d\nargc_ind %d\n", */
            /*        loop_data.to_submit, loop_data.to_wait, argc - loop_data.argc_ind); */
            break;
        }

        bool prepped = false;
        if (loop_data.argc_ind < argc && loop_data.cur_free_ind < QDT) {
            struct op_data *ptr =
                loop_data.op_data_ptrs[loop_data.cur_free_ind++];
            ret = prep_statx_data(ptr, argv[loop_data.argc_ind++]);
            if (ret == -1) {
                fprintf(stderr, "Failed to prepare ring data\n");
                goto err;
            }
            prep_op(&ring, ptr, &loop_data);
            prepped = true;
        }

        if (!splice_wait && prepped) continue;
        if (loop_data.to_submit > 0) {
            ret = io_uring_submit(&ring);
            if (ret < 0) {
                fprintf(stderr, "Failed to submit to ring: %s\n",
                        strerror(-ret));
                goto err;
            }
            loop_data.to_wait += loop_data.to_submit;
            loop_data.to_submit = 0;
        }

        struct io_uring_cqe *cqe;
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "Failed to get completion: %s\n",
                    strerror(-ret));
            goto err;
        }
        struct op_data *data = io_uring_cqe_get_data(cqe);
        if (cqe->res < 0) { // failed
            if (cqe->res == -EAGAIN) {
                prep_op(&ring, data, &loop_data);
            } else if (data->type != CLOSE) {
                //char *op = data->type == STATX ? "stat" : "open";
                char *op = NULL;
                if (data->type == STATX) op = "stat";
                else if (data->type == OPEN) op = "open";
                else if (data->type == FILE_TO_PIPE) op = "file_to_pipe";
                else if (data->type == PIPE_TO_SOCKET) op = "pipe_to_socket";
                fprintf(stdout, "Skipping %s. Failed to %s.\n", data->path,
                        op);
            }
        } else { // succeeded
            switch (data->type) {
            case STATX:
                if (S_ISREG(data->statx_buf.stx_mode)) {
                    // let's reuse the data pointer for async open
                    data->type = OPEN;
                    prep_op(&ring, data, &loop_data);
                } else if (S_ISDIR(data->statx_buf.stx_mode)) {
                    // not implemented yet
                } else {
                    fprintf(stdout, "Skipping %s. Not a regular file or "
                            "folder\n", data->path);
                }
                break;
            case OPEN:
                /* ret = send_file(data, cqe->res, sockfd); */
                /* if (ret != 0) { */
                /*     fprintf(stderr, "Failed to send file %s. Exiting\n", */
                /*             data->path); */
                /*     goto err; */
                /* } */
                /* data->type = CLOSE; */
                /* data->fd = cqe->res; */
                /* prep_op(&ring, data, &loop_data); */
                loop_data.op_data_ptrs[--loop_data.cur_free_ind] = data;
                if (file_to_pipe_ongoing) {
                    ++fds_to_splice_ind;
                    fds_to_splice[fds_to_splice_ind].fd = cqe->res;
                    fds_to_splice[fds_to_splice_ind].to_splice = data->statx_buf.stx_size;
                } else {
                    struct op_data *rsv1 = loop_data.op_data_ptrs[QD - 2];
                    struct op_data *rsv2 = loop_data.op_data_ptrs[QD - 1];
                    rsv1->type = FILE_TO_PIPE;
                    rsv1->fd = cqe->res;
                    rsv1->to_splice = data->statx_buf.stx_size;
                    rsv1->spliced = 0;
                    prep_op(&ring, rsv1, &loop_data);

                    if (!splice_wait) {
                        rsv2->type = PIPE_TO_SOCKET;
                        rsv2->fd = sockfd;
                        rsv2->spliced = 0;
                        prep_op(&ring, rsv2, &loop_data);
                        submitted_pipe = true;
                    }

                    ret = io_uring_submit(&ring);
                    if (ret < 0) {
                        fprintf(stderr, "Failed to submit to ring: %s\n",
                                strerror(-ret));
                        goto err;
                    }
                    //printf("file_to_pipe added\n");
                    loop_data.to_wait += loop_data.to_submit;
                    loop_data.to_submit = 0;
                    splice_wait = true;
                    file_to_pipe_ongoing = true;
                }
                break;
            case FILE_TO_PIPE:
                if (cqe->res == 0) {
                    close(pipes[1]);
                    break;
                }
                //printf("reached file_to_pipe\n");
                data->spliced += (uint64_t) cqe->res;
                piped += (uint64_t) cqe->res;
                if (data->spliced == data->to_splice) {
                    printf("reached equal tot piped %lu tot socketed %lu\n", piped, socketed);
                    if (fds_to_splice_ind >= 0 && next_splice_fd_ind <= fds_to_splice_ind) {
                        data->fd = fds_to_splice[next_splice_fd_ind].fd;
                        data->to_splice = fds_to_splice[next_splice_fd_ind].to_splice;
                        data->spliced = 0;
                        prep_op(&ring, data, &loop_data);
                        ret = io_uring_submit(&ring);
                        if (ret < 0) {
                            fprintf(stderr, "Failed to submit to ring: %s\n",
                                    strerror(-ret));
                            goto err;
                        }
                        next_splice_fd_ind++;
                        printf("file_to_pipe added from list\n");
                        loop_data.to_wait += loop_data.to_submit;
                        loop_data.to_submit = 0;
                        splice_wait = true;
                        file_to_pipe_ongoing = true;
                    } else {
                        //splice_wait = false;
                        file_to_pipe_ongoing = false;
                    }
                    //prep_op
                } else {
                    assert(data->spliced < data->to_splice);
                    prep_op(&ring, data, &loop_data);
                    ret = io_uring_submit(&ring);
                    if (ret < 0) {
                        fprintf(stderr, "Failed to submit to ring: %s\n",
                                strerror(-ret));
                        goto err;
                    }
                    loop_data.to_wait += loop_data.to_submit;
                    loop_data.to_submit = 0;
                    splice_wait = true;
                    file_to_pipe_ongoing = true;
                }
                break;
            case PIPE_TO_SOCKET:
                if (cqe->res == 0) {
                    close(pipes[0]);
                    break;
                }
                socketed += (uint64_t) cqe->res;
                prep_op(&ring, data, &loop_data);
                ret = io_uring_submit(&ring);
                if (ret < 0) {
                    fprintf(stderr, "Failed to submit to ring: %s\n",
                            strerror(-ret));
                    goto err;
                }
                loop_data.to_wait += loop_data.to_submit;
                loop_data.to_submit = 0;
                splice_wait = true;
                break;
            case CLOSE:
                loop_data.op_data_ptrs[--loop_data.cur_free_ind] = data;
                break;
            default:
                fprintf(stderr, "Unexpected op type\n");
                goto err;
            }
        }
        io_uring_cqe_seen(&ring, cqe);
        loop_data.to_wait--;
    }

    io_uring_queue_exit(&ring);
    close(sockfd);
    return 0;

 err:
    io_uring_queue_exit(&ring);
 err2:
    close(sockfd);
    return 1;
}
