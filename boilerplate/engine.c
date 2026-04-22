/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Clean working version for WSL/basic compilation.
 * Includes:
 * - CLI parsing
 * - bounded buffer
 * - logger thread
 * - simple UNIX socket client/server skeleton
 *
 * Advanced clone/container parts remain simplified.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    int server_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
} supervisor_ctx_t;

/* ===================================================== */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        " %s supervisor <base-rootfs>\n"
        " %s start <id> <rootfs> <command>\n"
        " %s run <id> <rootfs> <command>\n"
        " %s ps\n"
        " %s logs <id>\n"
        " %s stop <id>\n",
        prog, prog, prog, prog, prog, prog);
}

/* ===================================================== */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
    pthread_mutex_init(&buffer->mutex, NULL);
    pthread_cond_init(&buffer->not_empty, NULL);
    pthread_cond_init(&buffer->not_full, NULL);
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_mutex_destroy(&buffer->mutex);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_cond_destroy(&buffer->not_full);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/* ===================================================== */

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ===================================================== */

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    FILE *fp;
    char path[PATH_MAX];

    mkdir(LOG_DIR, 0755);

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        fp = fopen(path, "a");
        if (fp) {
            fwrite(item.data, 1, item.length, fp);
            fclose(fp);
        }
    }

    return NULL;
}

/* ===================================================== */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;

    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));

    bounded_buffer_init(&ctx.log_buffer);

    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    unlink(CONTROL_PATH);

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    listen(ctx.server_fd, 5);

    printf("Supervisor running...\n");
    printf("Socket: %s\n", CONTROL_PATH);

    while (1) {
        int client_fd;
        control_request_t req;
        control_response_t res;

        client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0)
            continue;

        read(client_fd, &req, sizeof(req));

        memset(&res, 0, sizeof(res));
        res.status = 0;

        switch (req.kind) {
        case CMD_START:
            snprintf(res.message, sizeof(res.message),
                     "Started container %s", req.container_id);
            break;

        case CMD_RUN:
            snprintf(res.message, sizeof(res.message),
                     "Run container %s", req.container_id);
            break;

        case CMD_PS:
            snprintf(res.message, sizeof(res.message),
                     "No active containers");
            break;

        case CMD_LOGS:
            snprintf(res.message, sizeof(res.message),
                     "Logs command for %s", req.container_id);
            break;

        case CMD_STOP:
            snprintf(res.message, sizeof(res.message),
                     "Stopped %s", req.container_id);
            break;

        default:
            snprintf(res.message, sizeof(res.message), "Unknown command");
        }

        write(client_fd, &res, sizeof(res));
        close(client_fd);
    }

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    return 0;
}

/* ===================================================== */

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t res;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    write(fd, req, sizeof(*req));
    read(fd, &res, sizeof(res));

    printf("%s\n", res.message);

    close(fd);
    return res.status;
}

/* ===================================================== */

static int cmd_simple(command_kind_t kind, int argc, char *argv[])
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = kind;

    if (argc > 2)
        strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    if (argc > 3)
        strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);

    if (argc > 4)
        strncpy(req.command, argv[4], sizeof(req.command) - 1);

    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    return send_control_request(&req);
}

/* ===================================================== */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_simple(CMD_START, argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_simple(CMD_RUN, argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_simple(CMD_PS, argc, argv);

    if (strcmp(argv[1], "logs") == 0)
        return cmd_simple(CMD_LOGS, argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_simple(CMD_STOP, argc, argv);

    usage(argv[0]);
    return 1;
}
