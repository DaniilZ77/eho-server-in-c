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

#define BUFFER_SIZE 4096
#define ALARM_PERIOD 5

int alarm_count = 0;
int message_count = 0;
int message_size = 0;

enum type {
    foreground,
    demon
};

struct config {
    char* fifo_name;
    char* out;
    enum type type;
};

void print_help(FILE* file) {
    fprintf(file, "-fifo_name: file name for named pipe\n");
    fprintf(file, "-type: one of foreground or demon\n");
    fprintf(file, "-out: output file (required for demon)\n");
}

struct config* read_config(int argc, char* argv[]) {
    struct config* cfg = (struct config*)calloc(0, sizeof(struct config));
    cfg->type = foreground;

    int i = 1;
    while (i < argc) {
        if (strcmp("-fifo_name", argv[i]) == 0) {
            if (argc > i + 1) {
                if (cfg->fifo_name != NULL) free(cfg->fifo_name);
                cfg->fifo_name = (char*)malloc(sizeof(char) * strlen(argv[i + 1]));
                strcpy(cfg->fifo_name, argv[i + 1]);
                ++i;
            } else {
                perror("invalid flag: fifo_name\n");
                return NULL;
            }
        } else if (strcmp("-type", argv[i]) == 0) {
            if (argc > i + 1) {
                if (strcmp(argv[i + 1], "foreground") == 0) cfg->type = foreground;
                else if (strcmp(argv[i + 1], "demon") == 0) cfg->type = demon;
                ++i;
            } else {
                perror("invalid flag: type\n");
                return NULL;
            }
        } else if (strcmp("-out", argv[i]) == 0) {
            if (argc > i + 1) {
                if (cfg->out != NULL) free(cfg->out);
                cfg->out = (char*)malloc(sizeof(char) * strlen(argv[i + 1]));
                strcpy(cfg->out, argv[i + 1]);
                ++i;
            } else {
                perror("invalid flag: out\n");
                return NULL;
            }
        }
        ++i;
    }
    return cfg;
}

void destroy_config(struct config* cfg) {
    free(cfg->fifo_name);
    free(cfg);
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

void close_fifo(FILE* fifo, FILE* out) {
    if (fclose(fifo) != 0) {
        fprintf(out, "failed to close fifo: %s\n", strerror(errno));
        exit(1);
    }
}

void print_stats(FILE *file) {
    fprintf(file, "message count: %d\n", message_count);
    fprintf(file, "message size: %d\n", message_size);
    fprintf(file, "alarm count: %d\n", alarm_count);
}

void shutdown(FILE* fifo, char* fifo_name, FILE* out) {
    int n;
    char buffer[BUFFER_SIZE];
    switch (app_status) {
    case 1:
        while (fifo != NULL && (n = fread(buffer, sizeof(char), BUFFER_SIZE, fifo)) > 0) {
            buffer[n] = '\0';
            fputs(buffer, out);
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
    struct config* cfg = read_config(argc, argv);
    if (cfg == NULL) exit(1);

    int fifo_status = setup_fifo(cfg->fifo_name);
    if (fifo_status != 0) exit(fifo_status);

    FILE* out = NULL;
    if (cfg->type == demon) {
        if (cfg->out == NULL) {
            perror("demon mode requires -out flag\n");
            exit(1);
        }
        out = fopen(cfg->out, "a");
        if (out == NULL) {
            perror("failed to open out\n");
            exit(1);
        }
        create_demon(out);
    } else out = stdout;

    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        fprintf(out, "failed to register sigterm handler: %s\n", strerror(errno));
        exit(1);
    }
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        fprintf(out, "failed to register sigint handler: %s\n", strerror(errno));
        exit(1);
    }
    if (sigaction(SIGQUIT, &sa, NULL) != 0) {
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

    setbuf(out, NULL);
    print_help(out);
    char buffer[BUFFER_SIZE];
    int n;
    while (1) {
        FILE* fifo = fopen(cfg->fifo_name, "r");
        if (fifo == NULL && errno == EINTR) {
            if (app_status == 3) {
                fprintf(out, "eho server working\n");
                app_status = 0;
            } else if (app_status == 4) {
                print_stats(out);
                app_status = 0;
            } else if (app_status == 5) {
                if (cfg->type != demon) {
                    if (cfg->out == NULL) {
                        perror("demon mode requires -out flag\n");
                        exit(1);
                    }
                    out = fopen(cfg->out, "a");
                    if (out == NULL) {
                        perror("failed to open out\n");
                        exit(1);
                    }
                    create_demon(out);
                    cfg->type = demon;
                }
                app_status = 0;
            } else {
                shutdown(fifo, cfg->fifo_name, out);
                break;
            }
        } else if (fifo == NULL) {
            fprintf(out, "failed to open fifo: %s\n", strerror(errno));
            exit(1);
        }

        while (fifo != NULL && (n = fread(buffer, sizeof(char), BUFFER_SIZE, fifo)) > 0) {
            buffer[n] = '\0';
            fputs(buffer, out);
            message_size += n;
        }

        ++message_count;

        if (fifo != NULL && n < 0 && errno == EINTR) {
            if (app_status == 3) {
                fprintf(out, "eho server working\n");
                app_status = 0;
            } else if (app_status == 4) {
                print_stats(out);
                app_status = 0;
            } else if (app_status == 5) {
                if (cfg->type != demon) {
                    if (cfg->out == NULL) {
                        perror("demon mode requires -out flag\n");
                        exit(1);
                    }
                    out = fopen(cfg->out, "a");
                    if (out == NULL) {
                        perror("failed to open out\n");
                        exit(1);
                    }
                    create_demon(out);
                    cfg->type = demon;
                }
                app_status = 0;
            } else {
                shutdown(fifo, cfg->fifo_name, out);
                close_fifo(fifo, out);
                break;
            }
        }

        if (fifo != NULL) close_fifo(fifo, out);
    }

    if (fclose(out) != 0) exit(1);
    destroy_config(cfg);
    exit(0);
}
