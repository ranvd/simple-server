#include "linenoise.h"

#ifndef SIMPLE_SERVER_CONSOLE_H
#define SIMPLE_SERVER_CONSOLE_H

typedef long int fd_t;

int console_start(fd_t);
int console_close(fd_t);

int commands_init(char *path);

void showall_cmd();

void completion(const char *buf, linenoiseCompletions *lc);
// char *hints(const char *buf, int *color, int *bold);

#endif /* SIMPLE_SERVER_CONSOLE_H */