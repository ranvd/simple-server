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

typedef struct __cmd_element {
    char name[LINENOISE_MAX_LINE];
    char fullname[LINENOISE_MAX_LINE];
    char *param;
    int (*operation)(struct __cmd_element param);
    struct __cmd_element *next;
} cmd_element;

static cmd_element *cmd_list = NULL;

int do_external_binary(cmd_element bin) {
    /* TODO: */
    return 1;
}

int add_command(cmd_element cmd) {
    cmd_element *new_cmd = malloc(sizeof(cmd_element));
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

int console_start(fd_t fd) {
    char *line;
    while (1) {
        line = linenoise("shell> ");
        linenoiseHistoryAdd(line);
        linenoiseHistorySave(".console.history");
        if (line == NULL) break;


    }
    return 1;
}

int console_close(fd_t fd) {
    /* TODO: */
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
                cmd.param = NULL;

                add_command(cmd);
            }
        }
    }

    /* register build-in function as command */
    return 1;
}

void showall_cmd() {
    for (cmd_element *head = cmd_list; head; head = head->next) {
        printf("%s\n", head->name);
    }
}

void completion(const char *buf, linenoiseCompletions *lc) {
    /* TODO: */
    return;
}

// char *hints(const char *buf, int *color, int *bold) {}