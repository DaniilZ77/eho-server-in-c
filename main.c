#include <stdatomic.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define BUFFER_SIZE 4096
#define ALARM_PERIOD 5
#define MAX_FIFO_NAME 256
#define MAX_OUT_NAME 256

int alarm_count = 0;
int message_count = 0;
int message_size = 0;

enum type {
    foreground,
    demon
};

struct config {
    char fifo_name[MAX_FIFO_NAME];
    char out[MAX_OUT_NAME];
    enum type type;
};

void print_help(FILE* file) {
    fprintf(file, "Options:\n");
    fprintf(file, "\t-fifo_name: file name for named pipe (max size: %d)\n", MAX_FIFO_NAME - 1);
    fprintf(file, "\t-type: one of foreground or demon\n");
    fprintf(file, "\t-out: output file (required for demon, max size: %d)\n", MAX_OUT_NAME - 1);
}

int read_config(struct config* cfg, int argc, char* argv[]) {
    if (cfg == NULL) {
        perror("cfg is null\n");
        return -1;
    }
    cfg->fifo_name[0] = '\0';
    cfg->out[0] = '\0';
    cfg->type = foreground;

    int i = 1;
    while (i < argc) {
        if (strcmp("-fifo_name", argv[i]) == 0) {
            if (argc > i + 1) {
                if (strlen(argv[i + 1]) >= MAX_FIFO_NAME) {
                    perror("fifo_name is too long\n");
                    return -1;
                }
                strcpy(cfg->fifo_name, argv[i + 1]);
                ++i;
            } else {
                perror("invalid flag: fifo_name\n");
                return -1;
            }
        } else if (strcmp("-type", argv[i]) == 0) {
            if (argc > i + 1) {
                if (strcmp(argv[i + 1], "foreground") == 0) cfg->type = foreground;
                else if (strcmp(argv[i + 1], "demon") == 0) cfg->type = demon;
                ++i;
            } else {
                perror("invalid flag: type\n");
                return -1;
            }
        } else if (strcmp("-out", argv[i]) == 0) {
            if (argc > i + 1) {
                if (strlen(argv[i + 1]) >= MAX_OUT_NAME) {
                    perror("out is too long\n");
                    return -1;
                }
                strcpy(cfg->out, argv[i + 1]);
                ++i;
            } else {
                perror("invalid flag: out\n");
                return -1;
            }
        }
        ++i;
    }

    if (cfg->fifo_name[0] == '\0') {
        perror("fifo_name is required\n");
        return -1;
    }

    return 0;
}

int setup_fifo(char* fifo_name) {
    int status = mkfifo(fifo_name, 0600);
    if (status != 0 && errno == EEXIST) {
        struct stat* st = (struct stat*)malloc(sizeof(struct stat));
        if (stat(fifo_name, st) != 0) {
            free(st);
            perror("stat error\n");
            return 1;
        }

        if (!S_ISFIFO(st->st_mode)) {
            free(st);
            perror("existing file is not fifo\n");
            return 1;
        }
        free(st);
    } else if (status != 0) {
        perror("failed to create fifo\n");
        return 1;
    }

    return 0;
}

void create_demon(FILE *out) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(out, "failed to fork: %s\n", strerror(errno));
        exit(1);
    }

    if (pid > 0) exit(0);

    if (setsid() < 0) {
        fprintf(out, "failed to set session: %s\n", strerror(errno));
        exit(1);
    }

    pid = fork();
    if (pid < 0) {
        fprintf(out, "failed to fork: %s\n", strerror(errno));
        exit(1);
    }

    if (pid > 0) exit(0);

    umask(0);
    if (chdir("/") != 0) {
        fprintf(out, "failed to change dir: %s\n", strerror(errno));
        exit(1);
    }

    if (close(0) != 0) {
        fprintf(out, "failed to close stdin: %s\n", strerror(errno));
        exit(1);
    }
    if (close(1) != 0) {
        fprintf(out, "failed to close stdout: %s\n", strerror(errno));
        exit(1);
    }
    if (close(2) != 0) {
        fprintf(out, "failed to close stderr: %s\n", strerror(errno));
        exit(1);
    }
}

volatile sig_atomic_t app_status = 0;

void signal_handler(int signal) {
    switch (signal) {
    case SIGINT:
        app_status = 1;
        break;
    case SIGTERM:
        app_status = 2;
        break;
    case SIGALRM:
        app_status = 3;
        ++alarm_count;
        alarm(ALARM_PERIOD);
        break;
    case SIGUSR1:
        app_status = 4;
        break;
    case SIGHUP:
        app_status = 5;
        break;
    }
}

void close_fifo(int fifo, FILE* out) {
    if (close(fifo) != 0) {
        fprintf(out, "failed to close fifo: %s\n", strerror(errno));
        exit(1);
    }
}

void print_stats(FILE *file) {
    fprintf(file, "message count: %d\n", message_count);
    fprintf(file, "message size: %d\n", message_size);
    fprintf(file, "alarm count: %d\n", alarm_count);
}

void init_demon_out(struct config* cfg, FILE** out) {
    if (cfg->out[0] == '\0') {
        perror("demon mode requires -out flag\n");
        exit(1);
    }
    *out = fopen(cfg->out, "w");
    if (*out == NULL) {
        perror("failed to open out\n");
        exit(1);
    }
    setbuf(*out, NULL);
}

void demonize(struct config *cfg, FILE* out) {
    if (cfg->type != demon) {
        init_demon_out(cfg, &out);
        create_demon(out);
        fprintf(out, "sighup signal: switching to demon mode\n");
        print_stats(out);
        cfg->type = demon;
    }
}

void shutdown(int fifo, char* fifo_name, FILE* out, struct config* cfg) {
    int n;
    char buffer[BUFFER_SIZE + 1];
    switch (app_status) {
    case 1:
        while (1) {
            while (fifo >= 0 && (n = read(fifo, buffer, BUFFER_SIZE)) > 0) {
                buffer[n] = '\0';
                fputs(buffer, out);
                message_size += n;
            }
            if (n < 0 && errno == EINTR) {
                if (app_status == 3) {
                    fprintf(out, "eho server working\n");
                    app_status = 0;
                } else if (app_status == 4) {
                    print_stats(out);
                    app_status = 0;
                } else if (app_status == 5) {
                    demonize(cfg, out);
                    app_status = 0;
                }
            } else break;
        }
        fprintf(out, "got sigint\n");
        break;
    case 2:
        fprintf(out, "got sigterm\n");
        break;
    }
    print_stats(out);
    if (unlink(fifo_name) != 0) exit(1);
}

int main(int argc, char* argv[]) {
    struct config cfg;
    int status = read_config(&cfg, argc, argv);
    if (status != 0) {
        print_help(stderr);
        exit(1);
    }

    int fifo_status = setup_fifo(cfg.fifo_name);
    if (fifo_status != 0) exit(fifo_status);

    FILE* out = NULL;
    if (cfg.type == demon) {
        init_demon_out(&cfg, &out);
        create_demon(out);
    } else out = stdout;

    struct sigaction sa = {.sa_handler = signal_handler};
    struct sigaction sa_ignore = {.sa_handler = SIG_IGN};
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        fprintf(out, "failed to register sigterm handler: %s\n", strerror(errno));
        exit(1);
    }
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        fprintf(out, "failed to register sigint handler: %s\n", strerror(errno));
        exit(1);
    }
    if (sigaction(SIGQUIT, &sa_ignore, NULL) != 0) {
        fprintf(out, "failed to register sigquit handler: %s\n", strerror(errno));
        exit(1);
    }
    if (sigaction(SIGALRM, &sa, NULL) != 0) {
        fprintf(out, "failed to register sigalrm handler: %s\n", strerror(errno));
        exit(1);
    }
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        fprintf(out, "failed to register sigusr1 handler: %s\n", strerror(errno));
        exit(1);
    }
    if (sigaction(SIGHUP, &sa, NULL) != 0) {
        fprintf(out, "failed to register sighup handler: %s\n", strerror(errno));
        exit(1);
    }

    alarm(ALARM_PERIOD);

    print_help(out);
    char buffer[BUFFER_SIZE + 1];
    int n;
    while (1) {
        int fifo = open(cfg.fifo_name, O_RDONLY, 0600);
        if (fifo < 0 && errno == EINTR) {
            if (app_status == 3) {
                fprintf(out, "eho server working\n");
                app_status = 0;
            } else if (app_status == 4) {
                print_stats(out);
                app_status = 0;
            } else if (app_status == 5) {
                if (cfg.type != demon) {
                    init_demon_out(&cfg, &out);
                    create_demon(out);
                    fprintf(out, "sighup signal: switching to demon mode\n");
                    print_stats(out);
                    cfg.type = demon;
                }
                app_status = 0;
            } else if (app_status != 6) {
                shutdown(fifo, cfg.fifo_name, out, &cfg);
                break;
            }
        } else if (fifo < 0) {
            fprintf(out, "failed to open fifo: %s\n", strerror(errno));
            exit(1);
        }

        while (fifo >= 0 && (n = read(fifo, buffer, BUFFER_SIZE)) > 0) {
            buffer[n] = '\0';
            fputs(buffer, out);
            message_size += n;
        }

        ++message_count;

        if (fifo >= 0 && n < 0 && errno == EINTR) {
            if (app_status == 3) {
                fprintf(out, "eho server working\n");
                app_status = 0;
            } else if (app_status == 4) {
                print_stats(out);
                app_status = 0;
            } else if (app_status == 5) {
                demonize(&cfg, out);
                app_status = 0;
            } else if (app_status != 6) {
                shutdown(fifo, cfg.fifo_name, out, &cfg);
                close_fifo(fifo, out);
                break;
            }
        }

        if (fifo >= 0) close_fifo(fifo, out);
    }

    if (fclose(out) != 0) exit(1);
    exit(0);
}
