#include "console.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "linenoise.h"
#include "utils.h"

#ifndef LINENOISE_MAX_LINE
/*
 * LINENOISE_MAX_LINE should be defined in linenoise.c,
 * if not, then define it.
 */
#define LINENOISE_MAX_LINE 4096
#endif /* LINENOISE_MAX_LINE */

typedef int (*cmd_callback)(struct __cmd_element, ...);

typedef struct __params {
    char value[256];
    struct __params *next;
} params;

typedef struct __cmd_element {
    char name[LINENOISE_MAX_LINE];
    char fullname[LINENOISE_MAX_LINE];
    cmd_callback operation;
    struct __params *params;
    struct __cmd_element *next;
} cmd_element;

typedef struct __waiting_cmd {
    int fd[2];
    char *param;
    struct __cmd_element *cmd_addr;
    struct __waiting_cmd *next;
} waiting_cmd;

static cmd_element *cmd_list = NULL;
static waiting_cmd *waiting_queue_Head = NULL;
static waiting_cmd *waiting_queue_Rear = NULL;

int do_external_binary(cmd_element bin_cmd, ...) {
    /* TODO: */
    return 1;
}

int do_pipe(cmd_element pipe, ...) {
    /* TODO: */
    return 1;
}
int do_quit(cmd_element pipe, ...) { return -1; }

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

/*
 * param: seperate by ":" or " "
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

cmd_element *check_cmd(char *cmd_name) {
    for (cmd_element *ptr = cmd_list; ptr; ptr = ptr->next) {
        if (strcmp(cmd_name, ptr->name) == 0) return ptr;
    }
    return NULL;
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
    last = s;

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

int console_start(fd_t fd) {
    char *line;
    while (1) {
        line = linenoise("shell> ");
        linenoiseHistoryAdd(line);
        linenoiseHistorySave(".console.history");
        if (line == NULL) break;

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
            wait_cmd.fd[0] = wait_cmd.fd[1] = 0;
            wait_cmd.cmd_addr = cmd_addr;
            wait_cmd.param = param;
            wait_cmd.next = NULL;
            if (append_queue(wait_cmd) == -1) return -1;

            free(split);
        }

        show_waiting_cmd();

        /* execute command in waiting queue */

        free(line);
    }
    return 1;
}

int console_close(fd_t fd) {
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

                if (add_command(cmd)) return -1;
            }
        }
    }
    free(p);

    /* register build-in function as command */
    if (add_builtin_command("|", "hello no a good day", do_pipe) == -1)
        return -1;
    if (add_builtin_command("quit", NULL, do_quit)) return -1;
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


void show_waiting_cmd(){
    for (waiting_cmd *from = waiting_queue_Rear; from; from = from->next){
        printf("%s->", from->cmd_addr->name);
    }
    printf("\n");
    return;
}