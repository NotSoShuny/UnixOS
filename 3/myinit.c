#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <getopt.h>

#define MAX_PROCESSES 64
#define MAX_PATH_LEN  256
#define MAX_ARGS      32
#define LOG_FILE      "/tmp/myinit.log"

typedef struct {
    char command_with_args[MAX_PATH_LEN];
    char stdin_file[MAX_PATH_LEN];
    char stdout_file[MAX_PATH_LEN];
} ProcessConfig;

pid_t pid_list[MAX_PROCESSES];
ProcessConfig proc_conf[MAX_PROCESSES];
int proc_count = 0;

int log_fd = -1;
char cfg_path[MAX_PATH_LEN];

volatile sig_atomic_t got_sighup = 0;

void write_log(const char *msg)
{
    if (log_fd < 0)
        return;
    write(log_fd, msg, strlen(msg));
    write(log_fd, "\n", 1);
}

int is_abs_path(const char *path)
{
    return (path != NULL && path[0] == '/');
}

void on_sighup(int sig)
{
    (void)sig;
    got_sighup = 1;
}

int read_config(const char *path)
{
    char line[MAX_PATH_LEN];
    char *tokens[MAX_ARGS + 2];
    int count;
    char *tok;
    int i;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        char msg[256];
        snprintf(msg, sizeof(msg), "ERROR: can't open config: %s", path);
        write_log(msg);
        return -1;
    }

    proc_count = 0;

    while (fgets(line, sizeof(line), fp) && proc_count < MAX_PROCESSES) {
        line[strcspn(line, "\r\n")] = '\0';

        if (line[0] == '\0' || line[0] == '#')
            continue;

        count = 0;
        tok = strtok(line, " ");
        while (tok && count < MAX_ARGS + 2) {
            tokens[count++] = tok;
            tok = strtok(NULL, " ");
        }

        if (count < 3) {
            write_log("WARN: invalid config line");
            continue;
        }

        if (!is_abs_path(tokens[0]) ||
            !is_abs_path(tokens[count - 2]) ||
            !is_abs_path(tokens[count - 1])) {
            write_log("WARN: not absolute path in config line");
            continue;
        }

        proc_conf[proc_count].command_with_args[0] = '\0';
        for (i = 0; i < count - 2; i++) {
            if (i > 0)
                strncat(proc_conf[proc_count].command_with_args, " ",
                        MAX_PATH_LEN - strlen(proc_conf[proc_count].command_with_args) - 1);
            strncat(proc_conf[proc_count].command_with_args, tokens[i],
                    MAX_PATH_LEN - strlen(proc_conf[proc_count].command_with_args) - 1);
        }

        snprintf(proc_conf[proc_count].stdin_file,
                 sizeof(proc_conf[proc_count].stdin_file),
                 "%s", tokens[count - 2]);
        snprintf(proc_conf[proc_count].stdout_file,
                 sizeof(proc_conf[proc_count].stdout_file),
                 "%s", tokens[count - 1]);

        pid_list[proc_count] = 0;
        proc_count++;
    }

    fclose(fp);

    {
        char msg[64];
        snprintf(msg, sizeof(msg), "INFO: read %d entries from config", proc_count);
        write_log(msg);
    }

    return proc_count;
}

void start_proc(int idx)
{
    pid_t cpid = fork();

    switch (cpid) {
    case -1:
        write_log("ERROR: fork() failed");
        return;

    case 0: {
        int fd_in, fd_out;
        char tmp[MAX_PATH_LEN];
        char *argv_arr[MAX_ARGS + 1];
        int argc = 0;
        char *t;

        fd_in = open(proc_conf[idx].stdin_file, O_RDONLY);
        if (fd_in < 0)
            fd_in = open("/dev/null", O_RDONLY);
        if (fd_in < 0)
            _exit(126);
        dup2(fd_in, STDIN_FILENO);
        if (fd_in != STDIN_FILENO)
            close(fd_in);

        fd_out = open(proc_conf[idx].stdout_file,
                      O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd_out < 0)
            fd_out = open("/dev/null", O_WRONLY);
        if (fd_out < 0)
            _exit(126);
        dup2(fd_out, STDOUT_FILENO);
        dup2(fd_out, STDERR_FILENO);
        if (fd_out != STDOUT_FILENO && fd_out != STDERR_FILENO)
            close(fd_out);

        snprintf(tmp, sizeof(tmp), "%s", proc_conf[idx].command_with_args);
        t = strtok(tmp, " ");
        while (t && argc < MAX_ARGS) {
            argv_arr[argc++] = t;
            t = strtok(NULL, " ");
        }
        argv_arr[argc] = NULL;

        execv(argv_arr[0], argv_arr);
        _exit(127);
    }

    default:
        pid_list[idx] = cpid;
        {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "INFO: started proc[%d] pid=%d cmd='%s'",
                     idx, cpid, proc_conf[idx].command_with_args);
            write_log(msg);
        }
        break;
    }
}

void start_all(void)
{
    int i;
    for (i = 0; i < proc_count; i++)
        start_proc(i);
}

void stop_all(void)
{
    int i, status;
    pid_t finished;

    for (i = 0; i < proc_count; i++) {
        if (pid_list[i] > 0)
            kill(pid_list[i], SIGTERM);
    }

    sleep(1);

    for (i = 0; i < proc_count; i++) {
        if (pid_list[i] <= 0)
            continue;

        finished = waitpid(pid_list[i], &status, WNOHANG);
        if (finished == 0) {
            kill(pid_list[i], SIGKILL);
            finished = waitpid(pid_list[i], &status, 0);
        }

        if (finished > 0) {
            char msg[256];
            if (WIFEXITED(status)) {
                snprintf(msg, sizeof(msg),
                         "INFO: proc[%d] pid=%d exited with code %d on reload",
                         i, finished, WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                snprintf(msg, sizeof(msg),
                         "INFO: proc[%d] pid=%d killed by signal %d on reload",
                         i, finished, WTERMSIG(status));
            } else {
                snprintf(msg, sizeof(msg),
                         "INFO: proc[%d] pid=%d finished on reload",
                         i, finished);
            }
            write_log(msg);
        }
        pid_list[i] = 0;
    }

    write_log("INFO: all children stopped");
}

void daemonize(void)
{
    int fd;
    struct rlimit flim;

    if (getppid() != 1) {
        signal(SIGTTOU, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);

        if (fork() != 0)
            exit(0);

        setsid();
    }

    getrlimit(RLIMIT_NOFILE, &flim);
    for (fd = 0; fd < (int)flim.rlim_max; fd++)
        close(fd);

    if (chdir("/") != 0)
        _exit(1);

    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2)
            close(fd);
    }

    log_fd = open(LOG_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (log_fd < 0)
        _exit(1);

    write_log("INFO: myinit started");
}

int main(int argc, char **argv)
{
    int opt;
    cfg_path[0] = '\0';

    while ((opt = getopt(argc, argv, "c:")) != -1) {
        switch (opt) {
        case 'c':
            snprintf(cfg_path, sizeof(cfg_path), "%s", optarg);
            break;
        default:
            fprintf(stderr, "usage: %s -c <config>\n", argv[0]);
            return 1;
        }
    }

    if (cfg_path[0] == '\0') {
        fprintf(stderr, "usage: %s -c <config>\n", argv[0]);
        return 1;
    }

    if (!is_abs_path(cfg_path)) {
        fprintf(stderr, "ERROR: config path must be absolute: %s\n", cfg_path);
        return 1;
    }

    daemonize();

    signal(SIGHUP, on_sighup);

    if (read_config(cfg_path) < 0) {
        write_log("ERROR: can't read config, exiting");
        return 1;
    }

    start_all();

    while (1) {
        int status;
        pid_t cpid = waitpid(-1, &status, 0);

        if (cpid == -1) {
            if (errno == EINTR) {
                if (got_sighup) {
                    got_sighup = 0;
                    write_log("INFO: got SIGHUP, reloading config");
                    stop_all();
                    proc_count = 0;
                    if (read_config(cfg_path) >= 0)
                        start_all();
                }
            }
            continue;
        }

        {
            int i;
            for (i = 0; i < proc_count; i++) {
                if (pid_list[i] != cpid)
                    continue;

                {
                    char msg[256];
                    if (WIFEXITED(status)) {
                        snprintf(msg, sizeof(msg),
                                 "INFO: proc[%d] pid=%d exited with code %d, restarting",
                                 i, cpid, WEXITSTATUS(status));
                    } else if (WIFSIGNALED(status)) {
                        snprintf(msg, sizeof(msg),
                                 "INFO: proc[%d] pid=%d killed by signal %d, restarting",
                                 i, cpid, WTERMSIG(status));
                    } else {
                        snprintf(msg, sizeof(msg),
                                 "INFO: proc[%d] pid=%d finished, restarting",
                                 i, cpid);
                    }
                    write_log(msg);
                }

                pid_list[i] = 0;
                start_proc(i);
                break;
            }
        }
    }

    return 0;
}