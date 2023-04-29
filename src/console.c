#include "console.h"

#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wait.h>

#include "linenoise.h"
#include "utils.h"

typedef struct __params {
    char value[256];
    struct __params *next;
} params;

typedef struct __waiting_cmd {
    char *param;
    int *read, *write;
    struct __cmd_element *cmd_addr;
    struct __waiting_cmd *next;
} waiting_cmd;

/*
 * cmd_list is mantain in singly linked-list.
 * waiting_cmd is a queue with singly linked-list.
 * pfd_list is mantain in circular linked-list.
 */
static cmd_element *cmd_list = NULL;
static waiting_cmd *waiting_queue_Head = NULL;
static waiting_cmd *waiting_queue_Rear = NULL;
static pfd_element *pfd_list = NULL;

int do_external_binary(cmd_element bin_cmd, char *params, ...) {
    char **params_list = parse_params(params, 1);
    params_list[0] = bin_cmd.name;

    execv(bin_cmd.fullname, params_list);
    return -1;
}

int do_pipe(cmd_element pipe, char *params, ...) {
    FILE *read, *write;
    if ((read = fdopen(STDIN_FILENO, "r")) == NULL) return -1;
    if ((write = fdopen(STDOUT_FILENO, "w")) == NULL) return -1;
    int c;
    while ((c = fgetc(read)) != EOF) {
        fputc(c, write);
    }

    return 0;
}

int do_quit(cmd_element pipe, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    pid_t parent = va_arg(ap, pid_t);
    va_end(ap);

    kill(parent, SIGINT);
    exit(EXIT_SUCCESS);
    return -1;
}

/*
 * This will append command(cmd) into cmd_list.
 */
int add_command(cmd_element cmd) {
    cmd_element *new_cmd = malloc(sizeof(cmd_element));
    if (new_cmd == NULL) return -1;

    memcpy(new_cmd, &cmd, sizeof(cmd_element));
    new_cmd->next = NULL;

    if (!cmd_list) {
        cmd_list = new_cmd;
        return 0;
    }

    new_cmd->next = cmd_list;
    cmd_list = new_cmd;
    return 0;
}

int add_param(cmd_element *cmd, char *param) {
    params *p = malloc(sizeof(params));
    if (p == NULL) return -1;

    strncpy(p->value, param, 256);
    p->next = NULL;

    if (!cmd->params) {
        cmd->params = p;
        return 0;
    }

    p->next = cmd->params;
    cmd->params = p;
    return 0;
}

/*
 * param: each parameter should seperate by ":" or " ".
 */
int add_builtin_command(char *cmd_name, char *param, cmd_callback operation) {
    cmd_element cmd;
    strcpy(cmd.name, cmd_name);
    strcpy(cmd.fullname, cmd_name);
    cmd.operation = operation;
    cmd.params = NULL;

    /* dealing with parameter */
    if (param) {
        char *p = strdup(param);
        char *split = strtok(p, ": ");

        for (; split; split = strtok(NULL, ": ")) {
            if (add_param(&cmd, split)) return -1;
        }
        free(p);
    }
    return add_command(cmd);
}

int append_queue(waiting_cmd cmd) {
    waiting_cmd *new_cmd = malloc(sizeof(waiting_cmd));
    if (new_cmd == NULL) return -1;

    memcpy(new_cmd, &cmd, sizeof(waiting_cmd));
    new_cmd->next = NULL;

    if (waiting_queue_Head == NULL) {
        waiting_queue_Head = new_cmd;
        waiting_queue_Rear = new_cmd;
        return 0;
    }

    waiting_queue_Head->next = new_cmd;
    waiting_queue_Head = waiting_queue_Head->next;
    return 0;
}

cmd_element *check_cmd(char *cmd_name) {
    if (cmd_name == NULL) return NULL;
    for (cmd_element *ptr = cmd_list; ptr; ptr = ptr->next) {
        if (strcmp(cmd_name, ptr->name) == 0) return ptr;
    }
    return NULL;
}

/*
 * add_pfd() help mantain opened file descripter in circular linked-list.
 */
pfd_element *add_pfd(int fd[2]) {
    if (pfd_list == NULL) {
        pfd_list = malloc(sizeof(pfd_element));
        pfd_list->next = pfd_list->prev = pfd_list;
        pfd_list->read = fd[0];
        pfd_list->write = fd[1];
        return pfd_list;
    }

    pfd_element *new_pfd = malloc(sizeof(pfd_element));
    new_pfd->read = fd[0];
    new_pfd->write = fd[1];
    pfd_list->next->prev = new_pfd;
    new_pfd->next = pfd_list->next;

    new_pfd->prev = pfd_list;
    pfd_list->next = new_pfd;
    pfd_list = new_pfd;

    return pfd_list;
}

int close_pfd(pfd_element *pfd) {
    if (pfd == NULL) return 0;

    close(pfd->read);
    close(pfd->write);

    if (pfd_list == pfd) {
        pfd_list = pfd->prev;
    }
    if (pfd->prev == pfd) {
        free(pfd);
        pfd_list = NULL;
        return 0;
    }

    pfd->prev->next = pfd->next;
    pfd->next->prev = pfd->prev;
    free(pfd);
    return 0;
}

/*
 * close all pfd except 0, 1, 2
 */
int close_all_pfd() {
    /* TODO: */
    for (pfd_element *cur = pfd_list; cur; cur = pfd_list) {
        close_pfd(cur);
    }
    return 0;
}

int free_all_waiting_cmd() {
    for (waiting_cmd *cur = waiting_queue_Rear; cur; cur = waiting_queue_Rear) {
        waiting_queue_Rear = waiting_queue_Rear->next;
        free(cur->param);
        free(cur);
    }
    waiting_queue_Head = NULL;
    return 0;
}

int exec_all_waiting_cmd() {
    if (waiting_queue_Rear == NULL) return 0;

    for (waiting_cmd *cur_cmd = waiting_queue_Rear; cur_cmd;
         cur_cmd = cur_cmd->next) {
        /* pipe in no need for last command. */
        if (cur_cmd->next) {
            int fd[2];
            if (pipe(fd) == -1) {
                perror("pipe");
                exit(EXIT_FAILURE);
            }

            pfd_element *cur_pfd = add_pfd(fd);
            cur_cmd->write = &cur_pfd->write;
            if (cur_cmd->next) {
                cur_cmd->next->read = &cur_pfd->read;
            }
        }

        pid_t child, parent = getpid();
        if ((child = fork()) == -1) {
            perror("fork");
            exit(EXIT_FAILURE);
        }

        if (child == 0) {
            /* child process */
            if (cur_cmd->read) dup2(*cur_cmd->read, STDIN_FILENO);
            if (cur_cmd->write) dup2(*cur_cmd->write, STDOUT_FILENO);
            close_all_pfd();

            int success = cur_cmd->cmd_addr->operation(*cur_cmd->cmd_addr,
                                                       cur_cmd->param, parent);
            exit(!(!success));
        }
        // showall_pfd();
    }

    close_all_pfd();
    free_all_waiting_cmd();
    while (wait(NULL) != -1)
        ;

    // showall_pfd();
    return 0;
}

/*
 * The usage of cmdtok is similar to the strtok but the different is
 * you need to free() the return string(char*) every time before you call.
 */
char *cmdtok(char *s, char *special_sign) {
    static char *last;
    char *cmd;

    if (s) last = s;
    if (s == NULL && (s = last) == NULL) return NULL;

    char c;
    // eliminate the space from begin
    for (; (c = *s); s++) {
        if (c == ' ')
            continue;
        else
            break;
    }

    if (*s == 0) {
        last = NULL;
        return NULL;
    }

    // check if *s is special_sign and eliminate the consecutive special_sign.
    for (char *spec = special_sign; (c = *spec++) != 0;) {
        if (*s == c) {
            if ((cmd = malloc(sizeof(char) + 1)) == NULL) {
                return NULL;
            }
            cmd[0] = c;
            cmd[1] = '\0';
            last = ++s;

            return cmd;
        }
    }

    // if not in special_sign, read following command to next special_sign
    for (; (c = *s) != 0; s++) {
        for (char *spec = special_sign; *spec != 0; spec++) {
            if (c == *spec) {
                int length = s - last;
                if ((cmd = malloc(length + 1)) == NULL) {
                    return NULL;
                }
                memcpy(cmd, last, length);
                cmd[length] = '\0';
                last = s;

                return cmd;
            }
        }
    }

    int length = s - last;
    if ((cmd = malloc(length + 1)) == NULL) {
        return NULL;
    }
    memcpy(cmd, last, length);
    cmd[length] = '\0';
    last = s;
    return cmd;
}

int console_start(fd_t fd_in, fd_t fd_out, fd_t fd_err) {
    if (fd_in > 2) dup2(fd_in, STDIN_FILENO);
    if (fd_out > 2) dup2(fd_out, STDOUT_FILENO);
    if (fd_err > 2) dup2(fd_err, STDERR_FILENO);

    char *line;
    while (1) {
        line = linenoise("shell> ");
        if (line == NULL) break;

        linenoiseHistoryAdd(line);
        linenoiseHistorySave(".console.history");

        /* put command into waiting queue */
        char *split = cmdtok(line, "|");
        for (; split; split = cmdtok(NULL, "|")) {
            char *name = strtok(split, " ");
            char *param = strtok(NULL, "");

            cmd_element *cmd_addr;
            if ((cmd_addr = check_cmd(name)) == NULL) {
                printf("command not found: %s DOESN'T EXIST\n", split);
                break;
            }

            waiting_cmd wait_cmd;
            wait_cmd.read = wait_cmd.write = NULL;
            wait_cmd.cmd_addr = cmd_addr;
            if (param) {
                wait_cmd.param = strdup(param);
            } else {
                wait_cmd.param = NULL;
            }
            wait_cmd.next = NULL;
            if (append_queue(wait_cmd) == -1) return -1;

            free(split);
        }

        // showall_waiting_cmd();
        /* execute command in waiting queue */
        free(line);
        if (exec_all_waiting_cmd() == -1) {
            return -1;
        }
    }
    return 1;
}

int console_close(fd_t fd_in, fd_t fd_out, fd_t fd_err) {
    /* TODO: */
    printf("Closing: free all allocated resource\n");
    while (cmd_list) {
        cmd_element *free_ptr = cmd_list;
        cmd_list = cmd_list->next;

        while (free_ptr->params) {
            params *p = free_ptr->params;
            free_ptr->params = free_ptr->params->next;
            free(p);
        }
        free(free_ptr);
    }
    return 1;
}

int commands_init(char *path) {
    /* register all binary executable file in path */
    char *p = strdup(path);
    char *split = strtok(p, ":");

    for (; split; split = strtok(NULL, ":")) {
        DIR *dir;

        if ((dir = opendir(split)) == NULL) {
            perror("opendir");
        }

        struct dirent *file;
        while ((file = readdir(dir))) {
            char fullname[LINENOISE_MAX_LINE] = {0};

            strcat(strcat(strcpy(fullname, split), "/"), file->d_name);
            if (is_executable(fullname)) {
                /* initialize cmd value */
                cmd_element cmd;
                strcpy(cmd.fullname, fullname);
                strcpy(cmd.name, file->d_name);
                cmd.operation = do_external_binary;
                cmd.params = NULL;

                if (add_command(cmd) == -1) return -1;
            }
        }
        closedir(dir);
    }
    free(p);

    /* register build-in function as command */
    if (add_builtin_command("|", NULL, do_pipe) == -1) return -1;
    if (add_builtin_command("quit", "now:2min:3min", do_quit)) return -1;
    if (add_builtin_command("server", "start:tell", do_server)) return -1;
    return 1;
}

void completion(const char *buf, linenoiseCompletions *lc) {
    /* TODO: */
    return;
}

// char *hints(const char *buf, int *color, int *bold) {}

// DEBUG
void showall_cmd() {
    for (cmd_element *head = cmd_list; head; head = head->next) {
        printf("%s\n", head->name);
        for (params *p = head->params; p; p = p->next) {
            printf("  < %s >\n", p->value);
        }
    }
    return;
}

void showall_waiting_cmd() {
    for (waiting_cmd *from = waiting_queue_Rear; from; from = from->next) {
        printf("%s->", from->cmd_addr->name);
    }
    printf("\n");
    return;
}

void showall_pfd() {
    for (pfd_element *from = pfd_list->next; from != pfd_list;
         from = from->next) {
        printf("%d, %d <--> ", from->read, from->write);
    }
    printf("%d, %d\n", pfd_list->read, pfd_list->write);
    return;
}