#include <unistd.h>

#include "console.h"

#define CONSOLE_HISTORY_LEN 50

int main(void) {
    /*
     * command config
     * commands_init(PATH): initial command list from PATH, seperate by ":".
     */
    commands_init("./bin:");

    /* 
     * linenoise config. Add all registered command in to linenoise completion
     * and hints. Also, load command history and set history length.
     */
    // linenoiseSetCompletionCallback(completion);
    // linenoiseSetHintsCallback(hints);
    linenoiseHistoryLoad(".console.history");
    linenoiseHistorySetMaxLen(CONSOLE_HISTORY_LEN);

    showall_cmd();
    int success = 1;
    success = success && console_start(STDIN_FILENO);
    success = success && console_close(STDIN_FILENO);

    return !success;
}