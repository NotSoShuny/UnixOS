#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <getopt.h>

static volatile sig_atomic_t stop_requested = 0;
static unsigned long successful_locks_counter = 0;

static void error_exit(const char *context_msg) {
    perror(context_msg);
    exit(EXIT_FAILURE);
}

static void handle_sigint(int sig) {
    (void)sig;
    stop_requested = 1;
}

// Устанавливает обработчик SIGINT
static void setup_signal_handler(void) {
    struct sigaction signal_config;
    memset(&signal_config, 0, sizeof(signal_config)); // Обнуляем структуру
    signal_config.sa_handler = handle_sigint; // При получении SIGINT вызываем обработчик
    sigemptyset(&signal_config.sa_mask);
    signal_config.sa_flags = 0; // Прерванные системные вызовы sleep read и тд не будут перезапущены после обработки
    if (sigaction(SIGINT, &signal_config, NULL) < 0) // Регистируем новый обработчик, не сохраняя предыдущего
        error_exit("sigaction");
}

// Дописывает строку со статистикой в результирующий файл, O_APPEND для атомарности, строки от разных процессов не перемешаются
static void append_lock_stat(const char *stats_path) {
    int stat_file = open(stats_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (stat_file < 0)
        error_exit("open stats");

    char line[64];
    int line_len = snprintf(line, sizeof(line), "PID %d: %lu locks\n", getpid(), successful_locks_counter);
    // Формат: "PID номер: количество locks\n"
    if (write(stat_file, line, line_len) < 0)
        error_exit("write stats");

    close(stat_file);
}

// Записывает метку с PID в начало общего файла и возвращает дескриптор -1 при ошибке, >=0 при успехе
static int write_pid_to_shared(const char *file_path) { 
    int work_file = open(file_path, O_RDWR | O_CREAT | O_TRUNC, 0644); // Очищаем файл перед записью, используя O_TRUNC
    if (work_file < 0)
        return -1;

    char mark[64];
    int mark_len = snprintf(mark, sizeof(mark), "PID %d was here\n", getpid());

    if (write(work_file, mark, mark_len) < 0) {
        close(work_file);
        return -1;
    }

    return work_file;
}

// Читает общий файл
static void read_sharedfile(int work_file) {
    if (lseek(work_file, 0, SEEK_SET) < 0)
        return;

    char read_buf[64];
    ssize_t bytes_read = read(work_file, read_buf, sizeof(read_buf) - 1);
    if (bytes_read > 0)
        read_buf[bytes_read] = '\0';
}

// Вызывается внутри критической секции, во время существования .lck файла, блокирует общий файл на одну секунду
static void lock_shared_file(const char *file_path) {
    int work_file = write_pid_to_shared(file_path);
    if (work_file < 0)
        return;

    read_sharedfile(work_file);
    close(work_file);
    sleep(1);
}

// Читает PID из lock-файла и сравнивает с PID текущего процесса
static int compare_PID(const char *lock_path) {
    int lock_fd = open(lock_path, O_RDONLY);
    if (lock_fd < 0)
        return 0;

    char pid_buf[32];
    ssize_t bytes_read = read(lock_fd,pid_buf, sizeof(pid_buf) - 1); // Оставили место для \0
    close(lock_fd);

    if (bytes_read <= 0)
        return 0;

    pid_buf[bytes_read] = '\0';
    return atoi(pid_buf) == getpid(); // если PID совпал, то 1, иначе 0
}

// Записывает PID текущего процесса в lock-файл
static int write_pid_to_lockfile(int lock_fd, const char *lock_path) {
    char pid_str[32];
    int pid_len = snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());

    if (write(lock_fd, pid_str, pid_len) < 0) { // Пытаемся записать PID в .lck файл
        close(lock_fd);
        unlink(lock_path); // Удаляем пустой .lck, блокировку никто не держит, т.к. запись неудачная
        return -1;
    }

    close(lock_fd); // Записали PID, .lck файл сохранился и закрылся
    return 0;
}

static int try_create_lockfile(const char *lock_path) {
    int lock_fd = open(lock_path, O_CREAT | O_EXCL | O_WRONLY, 0644);

    if (lock_fd >= 0) { // Файл создан, записали свой PID
        if (write_pid_to_lockfile(lock_fd, lock_path) < 0)
            return 0; // Не записали PID, файл удалён
        return 1; // Блокировка захвачена, PID записан
    }

    if (errno == EEXIST)
        return 0; // Файл уже существует, значит кто-то держит блокировку, сообщаем acquire_lock что нужно подождать

    error_exit("open lockfile");
    return -1;
}

static int acquire_lock(const char *lock_path) {
    while (!stop_requested) {
        if (try_create_lockfile(lock_path))
            return 1;

        usleep(10000); // Ждём перед новой попыткой
    }
    return 0;
}

static int release_lock(const char *lock_path) {
    if (!compare_PID(lock_path)) {
        fprintf(stderr, "PID %d: lock corrupted (not our PID)\n", getpid());
        return -1;
    }

    // Удаляем .lck, чтобы другие процессы могли захватить блокировку
    if (unlink(lock_path) < 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "PID %d: lock file disappeared before unlink\n",
                    getpid());
            return -1;
        }
        error_exit("unlink lockfile");
    }
    return 0;
}

int main(int argc, char *argv[]) {
    char *target_file = NULL;
    char *stats_file  = "stats.txt";

    int option;
    while ((option = getopt(argc, argv, "f:s:")) != -1) {
        switch (option) {
        case 'f':
            target_file = optarg;
            break;
        case 's':
            stats_file = optarg;
            break;
        default:
            fprintf(stderr, "Usage: %s -f file [-s statsfile]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (!target_file) {
        fprintf(stderr, "Error: -f <file> is required\n");
        return EXIT_FAILURE;
    }

    setup_signal_handler();

    char lock_path[256]; 
    snprintf(lock_path, sizeof(lock_path), "%s.lck", target_file); // Формируем путь к lck-файлу

    while (!stop_requested) {
        if (!acquire_lock(lock_path)) // Ждём и захватываем блокировку
            break;

        lock_shared_file(target_file); 

        if (release_lock(lock_path) < 0) { // Освобождаем блокировку и записываем кто там был
            append_lock_stat(stats_file);
            exit(EXIT_FAILURE);
        }

        successful_locks_counter++; 
        usleep(10000); // Иначе этот же процесс захватит блокировку, а так будет +- равномерность
    }

    append_lock_stat(stats_file); // Сохраняем статистику
    return 0;
}