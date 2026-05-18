#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <getopt.h>

#define OUTPUT_BUFFER_SIZE 512
#define RESPONSE_BUFFER_SIZE 128

int send_all_data(int socket_fd, const char *buffer, int length)
{
    int bytes_sent = 0;

    while (bytes_sent < length) {
        ssize_t result = write(socket_fd, buffer + bytes_sent, length - bytes_sent);

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

int read_server_response(int socket_fd, char *buffer, int max_length)
{
    int position = 0;

    while (position < max_length - 1) {
        char character;
        ssize_t result = read(socket_fd, &character, 1);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (result == 0) {
            return -1;
        }

        if (character == '\n') {
            buffer[position] = '\0';
            return position;
        }

        buffer[position++] = character;
    }

    buffer[position] = '\0';
    return position;
}

int flush_buffer_and_get_responses(int socket_fd,
                                   char *output_buffer,
                                   int *output_buffer_len,
                                   int *pending_response_count,
                                   int show_responses)
{
    int i;

    if (*output_buffer_len > 0) {
        if (send_all_data(socket_fd, output_buffer, *output_buffer_len) < 0) {
            return -1;
        }
        *output_buffer_len = 0;
    }

    for (i = 0; i < *pending_response_count; i++) {
        char response[RESPONSE_BUFFER_SIZE];

        if (read_server_response(socket_fd, response, sizeof(response)) < 0) {
            return -1;
        }

        if (show_responses) {
            printf("%s\n", response);
            fflush(stdout);
        }
    }

    *pending_response_count = 0;
    return 0;
}

int main(int argc, char **argv)
{
    char config_file_path[256] = "config";
    char socket_path[256] = "";
    char *input_file_path = NULL;
    char *delay_log_path = NULL;
    FILE *config_file;
    FILE *input_file = stdin;
    struct sockaddr_un server_addr;
    int socket_fd;
    double max_delay_sec = 0.0;
    double total_delay_sec = 0.0;
    int client_id = 0;
    int show_responses = 0;
    int read_from_stdin = 1;
    int opt;

    while ((opt = getopt(argc, argv, "c:f:d:i:l:p")) != -1) {
        switch (opt) {
        case 'c':
            snprintf(config_file_path, sizeof(config_file_path), "%s", optarg);
            break;
        case 'f':
            input_file_path = optarg;
            read_from_stdin = 0;
            break;
        case 'd':
            max_delay_sec = atof(optarg);
            break;
        case 'i':
            client_id = atoi(optarg);
            break;
        case 'l':
            delay_log_path = optarg;
            break;
        case 'p':
            show_responses = 1;
            break;
        default:
            fprintf(stderr,
                    "usage: %s [-c config] [-f input_file] [-d delay] [-i id] [-l delay_log] [-p]\n",
                    argv[0]);
            return 1;
        }
    }

    config_file = fopen(config_file_path, "r");
    if (config_file == NULL) {
        perror("fopen config");
        return 1;
    }

    if (fgets(socket_path, sizeof(socket_path), config_file) == NULL) {
        fprintf(stderr, "config file is empty\n");
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

    if (!read_from_stdin) {
        input_file = fopen(input_file_path, "r");
        if (input_file == NULL) {
            perror("fopen input file");
            return 1;
        }
    }

    srand((unsigned int)(time(NULL) ^ getpid()));

    socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        if (!read_from_stdin) {
            fclose(input_file);
        }
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strcpy(server_addr.sun_path, socket_path);

    if (connect(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(socket_fd);
        if (!read_from_stdin) {
            fclose(input_file);
        }
        return 1;
    }

    {
        char output_buffer[OUTPUT_BUFFER_SIZE];
        int buffer_length = 0;
        int pending_responses = 0;
        int bytes_in_current_chunk = 0;
        int chunk_size_threshold = (rand() % 255) + 1;
        int character;

        while ((character = fgetc(input_file)) != EOF) {
            char byte = (char)character;

            output_buffer[buffer_length++] = byte;
            bytes_in_current_chunk++;

            if (byte == '\n') {
                pending_responses++;
            }

            if (read_from_stdin && byte == '\n') {
                if (flush_buffer_and_get_responses(socket_fd,
                                                   output_buffer,
                                                   &buffer_length,
                                                   &pending_responses,
                                                   show_responses) < 0) {
                    perror("socket exchange");
                    close(socket_fd);
                    if (!read_from_stdin) {
                        fclose(input_file);
                    }
                    return 1;
                }
            }

            if (buffer_length == (int)sizeof(output_buffer)) {
                if (flush_buffer_and_get_responses(socket_fd,
                                                   output_buffer,
                                                   &buffer_length,
                                                   &pending_responses,
                                                   show_responses) < 0) {
                    perror("socket exchange");
                    close(socket_fd);
                    if (!read_from_stdin) {
                        fclose(input_file);
                    }
                    return 1;
                }
            }

            if (max_delay_sec > 0.0 && bytes_in_current_chunk >= chunk_size_threshold) {
                double delay_value;
                struct timespec sleep_time;

                if (flush_buffer_and_get_responses(socket_fd,
                                                   output_buffer,
                                                   &buffer_length,
                                                   &pending_responses,
                                                   show_responses) < 0) {
                    perror("socket exchange");
                    close(socket_fd);
                    if (!read_from_stdin) {
                        fclose(input_file);
                    }
                    return 1;
                }

                delay_value = max_delay_sec * ((double)rand() / (double)RAND_MAX);

                sleep_time.tv_sec = (time_t)delay_value;
                sleep_time.tv_nsec = (long)((delay_value - sleep_time.tv_sec) * 1000000000.0);

                nanosleep(&sleep_time, NULL);
                total_delay_sec += delay_value;

                bytes_in_current_chunk = 0;
                chunk_size_threshold = (rand() % 255) + 1;
            }
        }

        if (flush_buffer_and_get_responses(socket_fd,
                                           output_buffer,
                                           &buffer_length,
                                           &pending_responses,
                                           show_responses) < 0) {
            perror("socket exchange");
            close(socket_fd);
            if (!read_from_stdin) {
                fclose(input_file);
            }
            return 1;
        }
    }

    if (!read_from_stdin) {
        fclose(input_file);
    }

    if (shutdown(socket_fd, SHUT_WR) < 0) {
        perror("shutdown");
        close(socket_fd);
        return 1;
    }

    {
        char temp_buffer[256];
        ssize_t bytes_read;

        do {
            bytes_read = read(socket_fd, temp_buffer, sizeof(temp_buffer));
        } while (bytes_read > 0 || (bytes_read < 0 && errno == EINTR));

        if (bytes_read < 0) {
            perror("read");
            close(socket_fd);
            return 1;
        }
    }

    close(socket_fd);

    if (delay_log_path != NULL) {
        FILE *delay_log = fopen(delay_log_path, "w");
        if (delay_log != NULL) {
            fprintf(delay_log, "client_id=%d total_delay=%.6f\n",
                    client_id, total_delay_sec);
            fclose(delay_log);
        }
    }

    return 0;
}