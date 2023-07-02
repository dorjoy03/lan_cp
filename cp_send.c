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
#define OPEN_MODE 0

enum op_type {
    STATX,
    OPEN,
    CLOSE
};

struct op_data {
    enum op_type type;
    char *argv_ptr;
    char *path;
    size_t len;
    int fd; // file descriptor for closing
    struct statx statx_buf;
};

struct loop_info {
    int to_submit;
    int to_wait;
    int argc_ind;
    int cur_free_ind; // current free index in op_data_ptrs
    struct op_data *op_data_ptrs[QD];
};

struct send_data {
    int fd;
    uint64_t size;
};

struct path_info {
    char *path;
    size_t len;
};

struct dir_info {
    struct path_info *dir_list;
    size_t len;
    size_t cur;
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

static int send_file(struct send_data *data, int sockfd) {
    size_t lim = 2147479552;
    uint64_t bytes_left = data->size;
    while (bytes_left) {
        size_t max_count = bytes_left > lim ? lim : (size_t) bytes_left;
        ssize_t sent = sendfile(sockfd, data->fd, NULL, max_count);
        if (sent < 0) {
            perror("sendfile call failed");
            return -1;
        }
        bytes_left -= (uint64_t) sent;
    }

    return 0;
}

int submit_to_ring(struct io_uring *ring, struct loop_info *loop_data) {
    int ret = io_uring_submit(ring);
    if (ret < 0) {
        fprintf(stderr, "Failed to submit to ring: %s\n", strerror(-ret));
        return -1;
    }
    loop_data->to_wait += loop_data->to_submit;
    loop_data->to_submit = 0;
    return 0;
}

int add_to_dirs(const char *path, struct dir_info *dirs) {
    size_t mx = strlen(path);
    if (dirs->cur < dirs->len) {
        char *path = dirs->dir_list[dirs->cur].path;
        size_t len = dirs->dir_list[dirs->cur].len;
        char *str = NULL;
        if (len >= mx + 1) {
            str = path;
        } else {
            str = (char *) realloc(path, mx + 1);
            if (str == NULL) {
                perror("Failed to allocate memory");
                return -1;
            }
        }
        char *dst = strcpy(str, path);
        dirs->dir_list[dirs->cur].path = dst;
        dirs->dir_list[dirs->cur].len = mx + 1;
    } else {
        struct path_info *new_dir_list = realloc(dirs->dir_list, sizeof(struct path_info) * dirs->len * 2);
        if (new_dir_list == NULL) {
            perror("Failed to allocate memory");
            return -1;
        }
        dirs->dir_list = new_dir_list;
        dirs->len *= 2;
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

    struct send_data send_datas[QD];
    int free_send_data_ind = 0;

    struct path_info *dir_paths = (struct path_info*) realloc(NULL, sizeof(struct path_info) * QD);
    for (int i = 0; i < QD; ++i) {
        dir_paths[i].path = NULL;
        dir_paths[i].len = 0;
    }
    struct dir_info dirs = {dir_paths, QD, 0};

    while (true) {
        if (loop_data.to_submit == 0 && loop_data.to_wait == 0 &&
            loop_data.argc_ind == argc)
            break;

        if (loop_data.argc_ind < argc && loop_data.cur_free_ind < QD) {
            struct op_data *ptr =
                loop_data.op_data_ptrs[loop_data.cur_free_ind++];
            ret = prep_statx_data(ptr, argv[loop_data.argc_ind++]);
            if (ret == -1) {
                fprintf(stderr, "Failed to prepare ring data\n");
                goto err;
            }
            prep_op(&ring, ptr, &loop_data);
            continue;
        }

        if (loop_data.to_submit > 0) {
            ret = submit_to_ring(&ring, &loop_data);
            if (ret != 0)
                goto err;
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
                char *op = data->type == STATX ? "stat" : "open";
                fprintf(stdout, "Skipping %s. Failed to %s.\n", data->path, op);
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
                    ret = add_to_dirs(data->path, dirs);
                    if (ret == -1) {
                        fprintf(stderr, "Failed to add dir\n");
                        goto err;
                    }
                } else {
                    fprintf(stdout, "Skipping %s. Not a regular file or "
                            "directory\n", data->path);
                }
                break;
            case OPEN:
                if (free_send_data_ind == QD) {
                    for (int i = 0; i < QD; ++i) {
                        ret = send_file(&send_datas[i], sockfd);
                        if (ret != 0)
                            goto err;
                    }
                    free_send_data_ind = 0;
                }
                send_datas[free_send_data_ind].fd = cqe->res;
                send_datas[free_send_data_ind].size = data->statx_buf.stx_size;
                free_send_data_ind++;
                loop_data.op_data_ptrs[--loop_data.cur_free_ind] = data;
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

    for (int i = 0; i < free_send_data_ind; ++i) {
        ret = send_file(&send_datas[i], sockfd);
        if (ret != 0)
            goto err;
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
