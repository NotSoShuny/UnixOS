#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define DEFAULT_BLOCK 4096

void error_exit(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

int is_zero_block(const char *buffer, ssize_t length) {
    for (ssize_t i = 0; i < length; i++) {
        if (buffer[i] != 0) return 0;
    }
    return 1;
}

// Повторное чтение при EINTR
ssize_t my_read(int fd, void *buffer, size_t count) {
    ssize_t n;
    while ((n = read(fd, buffer, count)) < 0 && errno == EINTR);
    return n;
}

// Повторная запись при EINTR
void my_write(int fd, const void *buffer, size_t count) {
    size_t written = 0;
    while (written < count) {
        ssize_t n = write(fd, (char*)buffer + written, count - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            error_exit("write error");
        }
        written += n;
    }
}

int main(int args_count, char *args_array[]) {
    int block_size = DEFAULT_BLOCK;
    int option;

    while ((option = getopt(args_count, args_array, "b:")) != -1) { // Разбираем аргументы cmd, если видим -b значит дальше size
        if (option == 'b') block_size = atoi(optarg);
    }

    int input_fd, output_fd;
    if (args_count - optind == 1) {
        input_fd = STDIN_FILENO;
        output_fd = open(args_array[optind], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    } else if (args_count - optind == 2) {
        input_fd = open(args_array[optind], O_RDONLY);
        output_fd = open(args_array[optind + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    } else { // Если неправильное количество аргументов
        fprintf(stderr, "Usage: %s [-b blocksize] output_file\n", args_array[0]);
        fprintf(stderr, "       %s [-b blocksize] input_file output_file\n", args_array[0]);
        return 1;
    }

    if (input_fd < 0) error_exit("open input");
    if (output_fd < 0) error_exit("open output");

    char *buffer = malloc(block_size);
    if (!buffer) error_exit("malloc");

    ssize_t bytes_read;
    off_t total_bytes = 0;

    while ((bytes_read = my_read(input_fd, buffer, block_size)) > 0) {
        if (is_zero_block(buffer, bytes_read)) {
            if (lseek(output_fd, bytes_read, SEEK_CUR) == (off_t)-1) 
                error_exit("lseek");
        } else {
            my_write(output_fd, buffer, bytes_read);
        }
        total_bytes += bytes_read;
    }

    if (bytes_read < 0) error_exit("read error");

    if (ftruncate(output_fd, total_bytes) == -1) 
        error_exit("ftruncate");

    close(input_fd);
    close(output_fd);
    free(buffer);

    return 0;
}