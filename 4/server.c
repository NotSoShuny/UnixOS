#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <getopt.h>

#define MAX_CLIENTS 256
#define CLIENT_BUFFER_SIZE 4096
#define LINE_BUFFER_SIZE 64
#define LOG_FILE_PATH "/tmp/server.log"

typedef struct {
    int socket_fd;
    char data_buffer[CLIENT_BUFFER_SIZE];
    int data_length;
} ClientInfo;

long server_state = 0;

int log_file_fd = -1;

volatile sig_atomic_t shutdown_requested = 0;

long long get_current_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

void write_log(const char *message)
{
    if (log_file_fd < 0) {
        return;
    }

    write(log_file_fd, message, strlen(message));
    write(log_file_fd, "\n", 1);
}

void handle_shutdown_signal(int sig)
{
    (void)sig;
    shutdown_requested = 1;
}

int write_all_data(int fd, const char *buffer, int length)
{
    int bytes_sent = 0;

    while (bytes_sent < length) {
        ssize_t result = write(fd, buffer + bytes_sent, length - bytes_sent);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        bytes_sent += (int)result;
    }

    return 0;
}

void remove_client(ClientInfo clients[], int *client_count, int index)
{
    close(clients[index].socket_fd);

    if (index != *client_count - 1) {
        clients[index] = clients[*client_count - 1];
    }

    (*client_count)--;
}

int main(int argc, char **argv)
{
    char config_file_path[256] = "config";
    char socket_path[256] = "";
    int opt;

    FILE *config_file;
    int listen_socket_fd;
    struct sockaddr_un server_addr;

    struct pollfd poll_fds[MAX_CLIENTS + 1];
    ClientInfo clients[MAX_CLIENTS];
    int client_count = 0;

    while ((opt = getopt(argc, argv, "c:")) != -1) {
        switch (opt) {
        case 'c':
            snprintf(config_file_path, sizeof(config_file_path), "%s", optarg);
            break;
        default:
            fprintf(stderr, "usage: %s [-c config]\n", argv[0]);
            return 1;
        }
    }

    config_file = fopen(config_file_path, "r");
    if (config_file == NULL) {
        perror("fopen config");
        return 1;
    }

    if (fgets(socket_path, sizeof(socket_path), config_file) == NULL) {
        fprintf(stderr, "config is empty\n");
        fclose(config_file);
        return 1;
    }

    fclose(config_file);

    socket_path[strcspn(socket_path, "\r\n")] = '\0';

    if (socket_path[0] == '\0') {
        fprintf(stderr, "socket path is empty\n");
        return 1;
    }

    if (strlen(socket_path) >= sizeof(server_addr.sun_path)) {
        fprintf(stderr, "socket path too long: %s\n", socket_path);
        return 1;
    }

    log_file_fd = open(LOG_FILE_PATH, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (log_file_fd < 0) {
        perror("open log");
        return 1;
    }

    signal(SIGINT, handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);
    signal(SIGPIPE, SIG_IGN);

    unlink(socket_path);

    listen_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_socket_fd < 0) {
        perror("socket");
        close(log_file_fd);
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strcpy(server_addr.sun_path, socket_path);

    if (bind(listen_socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_socket_fd);
        close(log_file_fd);
        return 1;
    }

    if (listen(listen_socket_fd, 128) < 0) {
        perror("listen");
        close(listen_socket_fd);
        close(log_file_fd);
        unlink(socket_path);
        return 1;
    }

    {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "INFO: ts=%lld server started socket=%s state=%ld",
                 get_current_time_ms(), socket_path, server_state);
        write_log(msg);
    }

    while (!shutdown_requested) {
        int i;
        int ready_count;

        poll_fds[0].fd = listen_socket_fd;
        poll_fds[0].events = POLLIN;
        poll_fds[0].revents = 0;

        for (i = 0; i < client_count; i++) {
            poll_fds[i + 1].fd = clients[i].socket_fd;
            poll_fds[i + 1].events = POLLIN | POLLHUP | POLLERR;
            poll_fds[i + 1].revents = 0;
        }

        ready_count = poll(poll_fds, client_count + 1, 500);

        if (ready_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        if (ready_count == 0) {
            continue;
        }

        if (poll_fds[0].revents & POLLIN) {
            int new_client_fd = accept(listen_socket_fd, NULL, NULL);

            if (new_client_fd >= 0) {
                if (client_count >= MAX_CLIENTS) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "INFO: ts=%lld too many clients, reject fd=%d",
                             get_current_time_ms(), new_client_fd);
                    write_log(msg);
                    close(new_client_fd);
                } else {
                    char msg[256];
                    void *heap_break = sbrk(0);

                    clients[client_count].socket_fd = new_client_fd;
                    clients[client_count].data_length = 0;
                    clients[client_count].data_buffer[0] = '\0';
                    client_count++;

                    snprintf(msg, sizeof(msg),
                             "INFO: ts=%lld client connected fd=%d sbrk=%p clients=%d",
                             get_current_time_ms(), new_client_fd, heap_break, client_count);
                    write_log(msg);
                }
            }
        }

        for (i = 0; i < client_count; i++) {
            int should_skip_client = 0;
            short revents = poll_fds[i + 1].revents;

            if (!(revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }

            {
                char read_chunk[1024];
                ssize_t bytes_read = read(clients[i].socket_fd, read_chunk, sizeof(read_chunk));

                if (bytes_read <= 0) {
                    char msg[256];
                    void *heap_break = sbrk(0);

                    snprintf(msg, sizeof(msg),
                             "INFO: ts=%lld client disconnected fd=%d sbrk=%p clients=%d",
                             get_current_time_ms(), clients[i].socket_fd, heap_break, client_count - 1);
                    write_log(msg);

                    remove_client(clients, &client_count, i);
                    i--;
                    continue;
                }

                if (clients[i].data_length + (int)bytes_read > CLIENT_BUFFER_SIZE) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "WARN: ts=%lld client buffer overflow fd=%d",
                             get_current_time_ms(), clients[i].socket_fd);
                    write_log(msg);

                    remove_client(clients, &client_count, i);
                    i--;
                    continue;
                }

                memcpy(clients[i].data_buffer + clients[i].data_length,
                       read_chunk, (size_t)bytes_read);
                clients[i].data_length += (int)bytes_read;
            }

            while (1) {
                char *newline_position;
                char line[LINE_BUFFER_SIZE];
                int line_length;
                int bytes_consumed;
                int bytes_remaining;
                char *parse_end;
                long parsed_value;
                char response[64];
                int response_length;

                newline_position = memchr(clients[i].data_buffer, '\n', clients[i].data_length);
                if (newline_position == NULL) {
                    break;
                }

                line_length = (int)(newline_position - clients[i].data_buffer);

                if (line_length >= LINE_BUFFER_SIZE) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "WARN: ts=%lld line too long fd=%d",
                             get_current_time_ms(), clients[i].socket_fd);
                    write_log(msg);

                    remove_client(clients, &client_count, i);
                    i--;
                    should_skip_client = 1;
                    break;
                }

                memcpy(line, clients[i].data_buffer, (size_t)line_length);
                line[line_length] = '\0';

                bytes_consumed = line_length + 1;
                bytes_remaining = clients[i].data_length - bytes_consumed;

                if (bytes_remaining > 0) {
                    memmove(clients[i].data_buffer,
                            clients[i].data_buffer + bytes_consumed,
                            (size_t)bytes_remaining);
                }
                clients[i].data_length = bytes_remaining;

                errno = 0;
                parsed_value = strtol(line, &parse_end, 10);
                if (errno != 0 || parse_end == line || *parse_end != '\0') {
                    parsed_value = 0;
                }

                server_state += parsed_value;

                {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "RECV ts=%lld fd=%d str='%s' state=%ld",
                             get_current_time_ms(), clients[i].socket_fd, line, server_state);
                    write_log(msg);
                }

                response_length = snprintf(response, sizeof(response), "%ld\n", server_state);

                if (write_all_data(clients[i].socket_fd, response, response_length) < 0) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "INFO: ts=%lld write failed fd=%d",
                             get_current_time_ms(), clients[i].socket_fd);
                    write_log(msg);

                    remove_client(clients, &client_count, i);
                    i--;
                    should_skip_client = 1;
                    break;
                }

                {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "SEND ts=%lld fd=%d reply='%ld'",
                             get_current_time_ms(), clients[i].socket_fd, server_state);
                    write_log(msg);
                }
            }

            if (should_skip_client) {
                continue;
            }
        }
    }

    while (client_count > 0) {
        close(clients[client_count - 1].socket_fd);
        client_count--;
    }

    close(listen_socket_fd);
    unlink(socket_path);

    {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "INFO: ts=%lld server stopped final_state=%ld",
                 get_current_time_ms(), server_state);
        write_log(msg);
    }

    close(log_file_fd);
    return 0;
}