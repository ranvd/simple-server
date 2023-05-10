#include <sys/stat.h>

#ifndef SIMPLE_SERVER_UTILs_H
#define SIMPLE_SERVER_UTILs_H

static inline int is_executable(char *fullpath) {
    struct stat sb;
    return (stat(fullpath, &sb) == 0 && S_ISREG(sb.st_mode) &&
            (sb.st_mode & S_IXUSR));
}

static inline void exit_if_fail(int val, int fail, char *msg){
    if (val == fail){
        perror(msg);
        exit(EXIT_FAILURE);
    }
}

char **parse_params(char *params, int at);
#endif /* SIMPLE_SERVER_UTILs_H */