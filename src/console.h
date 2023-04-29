#include <sys/types.h>

#include "linenoise.h"
#ifndef LINENOISE_MAX_LINE
/*
 * LINENOISE_MAX_LINE should be defined in linenoise.c,
 * if not, then define it.
 */
#define LINENOISE_MAX_LINE 4096
#endif /* LINENOISE_MAX_LINE */

#ifndef SIMPLE_SERVER_CONSOLE_H
#define SIMPLE_SERVER_CONSOLE_H

typedef long int fd_t;
struct __cmd_element;
struct __pfd_element;

typedef int (*cmd_callback)(struct __cmd_element, char *, ...);

typedef struct __cmd_element {
    char name[LINENOISE_MAX_LINE];
    char fullname[LINENOISE_MAX_LINE];
    cmd_callback operation;
    struct __params *params;
    struct __cmd_element *next;
} cmd_element;

typedef struct __pfd_element {
    int read, write;
    struct __pfd_element *next, *prev;
} pfd_element;


int console_start(fd_t, fd_t, fd_t);
int console_close(fd_t, fd_t, fd_t);
int commands_init(char *path);

char *cmdtok(char *s, char *special_sign);

int add_command(struct __cmd_element cmd);
int add_builtin_command(char *cmd_name, char *param, cmd_callback operation);
struct __pfd_element *add_pfd(int fd[2]);

cmd_element *check_cmd(char *cmd_name);

int close_pfd(struct __pfd_element *pfd);
int close_all_pfd();

int free_all_waiting_cmd();


/* Debug */
void showall_cmd();
void showall_waiting_cmd();
void showall_pfd();

/* external function */
extern int do_server(struct __cmd_element pipe, char *params, ...);

void completion(const char *buf, linenoiseCompletions *lc);
// char *hints(const char *buf, int *color, int *bold);

#endif /* SIMPLE_SERVER_CONSOLE_H */