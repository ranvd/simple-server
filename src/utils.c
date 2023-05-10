#include <stdlib.h>
#include <string.h>

char **parse_params(char *params, int at) {
    /* parse parameter in to 2d-array */
    char **params_list = calloc(20, sizeof(char *));

    /*
     * Because the first call to strtok() should not be NULL.
     * According to the opensource from Apple:
     * https://opensource.apple.com/source/Libc/Libc-167/string.subproj/strtok.c.auto.html
     * We can see that, if the first call of strtok() is NULL,
     * The behavior may be Segmentation fault or some undefine behavior.
     */
    if (params == NULL) return params_list;

    char *split = strtok(params, " ");
    for (int c = at; split && c < 20; split = strtok(NULL, " "), c++) {
        params_list[c] = split;
    }
    return params_list;
}
