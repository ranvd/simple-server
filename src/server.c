#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define _GNU_SOURCE
#include <sys/socket.h>

#include "console.h"
#include "server.h"
#include "utils.h"

#ifndef EXIT_IF_FAIL
#define EXIT_IF_FAIL(v, fail, err_info) \
    if (v == fail) {                    \
        perror(err_info);               \
        exit(EXIT_FAILURE);             \
    }
#endif

#define SSC_NONAME 0   /* not named (not send request yet)*/
#define SSC_REQNAME 1  /* not named (sent request)*/
#define SSC_NAMED 2    /* named user & free to input command */
#define SSC_REQINPUT 4 /* send chat> already */
#define SSC_EXECING 8  /* user's input under executing */

int do_name(struct __cmd_element name, char *params, ...);

typedef struct __ipv4_server {
    int socket_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t len;
} ipv4_server;

/*
 * __chatroom_user is the data structure storing user info.
 * fd pointer point to the file descripter which belong to the user.
 * name is the user name.
 * status is the user status.
 *   - 0: there is no user info now (not yet send a msg to request user name)
 *   - 1: there is no user info now (already send a msg to request user name)
 *   - 2: named user.
 * next, prev pointers are next user and previous user.
 */
typedef struct __chatroom_user {
    pfd_element *fd;
    char name[1024];
    int status;
    pid_t console;
    struct __chatroom_user *next, *prev;
} chatroom_user;

/*
 * user_list is maintin in circular linked-list
 */
static chatroom_user *user_list = NULL;

/*
 * ipv4_config() will return socket fd with AF_INET and SOCK_STREAM.
 */
int ipv4_config(struct __ipv4_server *server, int ip, int port,
                int non_blocking) {
    int socket_fd = (non_blocking)
                        ? socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)
                        : socket(AF_INET, SOCK_STREAM, 0);

    EXIT_IF_FAIL(socket_fd, -1, "socket()");
    server->server_addr.sin_family = AF_INET;
    server->server_addr.sin_addr.s_addr = ip;
    server->server_addr.sin_port = port;
    server->client_addr.sin_family = AF_INET;
    server->client_addr.sin_addr.s_addr = 0;
    server->client_addr.sin_port = 0;

    EXIT_IF_FAIL(bind(socket_fd, (struct sockaddr *)&server->server_addr,
                      sizeof(struct sockaddr_in)),
                 -1, "bind()");
    EXIT_IF_FAIL(listen(socket_fd, 5), -1, "listen()");

    return socket_fd;
}

chatroom_user *add_user(pfd_element *pfd) {
    chatroom_user *new_user = calloc(1, sizeof(chatroom_user));
    if (new_user == NULL) return NULL;

    new_user->fd = pfd;

    if (user_list == NULL) {
        user_list = new_user;
        user_list->next = user_list->prev = user_list;
        return new_user;
    }

    new_user->next = user_list->next;
    new_user->prev = user_list;
    user_list->next = user_list->next->prev = new_user;
    user_list = new_user;
    /*
     * Note that, the "=" operation are right associate.
     * In conclusion, "user_list->next->prev = new_user" will
     * execute first, and "user_list->next" at the second.
     */
    return new_user;
}

chatroom_user *register_ipv4conn(int socket_fd, struct __ipv4_server *server,
                                 socklen_t *len, int flag) {
    int confd = accept4(socket_fd, &server->client_addr, len, flag);
    if (confd == -1) return NULL;

    int fd[2] = {confd, confd};
    pfd_element *new_pfd = add_pfd(fd, SSC_SOCK_CLIENT);

    return add_user(new_pfd);
}

chatroom_user *close_user(chatroom_user *user) {
    if (user == NULL) return NULL;

    close_pfd(user->fd);

    if (user_list == user) {
        user_list = user_list->prev;
    }
    if (user->prev == user) {
        free(user);
        user_list = NULL;
        return NULL;
    }

    chatroom_user *prev = user->prev;
    user->prev->next = user->next;
    user->next->prev = user->prev;
    free(user);
    return prev;
}

char *input_filter(char *input) {
    /* truncate the input until read invalid charater. */
    char *output = calloc(strlen(input), sizeof(char) + 1);
    char *start = output;
    for (; *input > 31 && *input < 127; input++) {
        *start++ = *input;
    }
    if (start == output) {
        free(output);
        return NULL;
    }
    return output;
}

int name_exist_in_system(char *name) {
    if (name == NULL) return 0;
    chatroom_user *tmp = user_list;
    do {
        if (strcmp(name, tmp->name) == 0) return 1;
        tmp = tmp->next;
    } while (tmp != user_list);
    return 0;
}

int init_waitingcmd(waiting_cmd *w_cmd, cmd_element *cmd, chatroom_user *user,
                    char *args) {
    w_cmd->read = w_cmd->write = NULL;
    w_cmd->cmd_addr = cmd;
    if (strcmp(cmd->name, "who") == 0 || strcmp(cmd->name, "tell") == 0 ||
        strcmp(cmd->name, "yell") == 0 || strcmp(cmd->name, "name") == 0) {
        w_cmd->additional_data = user;
    } else if (strcmp(cmd->name, "quit") == 0) {
        w_cmd->additional_data = (void *)getpid();
    }
    if (args) {
        w_cmd->param = strdup(args);
    } else {
        w_cmd->param = NULL;
    }
    w_cmd->next = NULL;
    return 0;
}

int user_stat_handler(chatroom_user *user, char *input) {
    switch (user->status) {
        case SSC_NONAME:
            ssize_t size =
                write(user->fd->write, "Who're you: ", sizeof("Who're you: "));
            if (size == -1) {
                perror("In server/server_start()");
            }
            user->status = SSC_REQNAME;
            return SSC_NONAME;
            break;
        case SSC_REQNAME:
            if (input) {
                char *neat_name = input_filter(input);
                if (neat_name == NULL) {
                    user->status = SSC_NONAME;
                    return SSC_REQNAME;
                }

                if (name_exist_in_system(neat_name)) {
                    user->status = SSC_NONAME;
                    dprintf(user->fd->write,
                            "User name exist, Please change\n");
                } else {
                    strncpy(user->name, neat_name, 1024);
                    user->status = SSC_NAMED;
                }

                free(neat_name);
            }
            return SSC_REQNAME;
            break;
        case SSC_NAMED:
            write(user->fd->write, user->name, strlen(user->name));
            write(user->fd->write, "> ", sizeof(">"));
            user->status = SSC_REQINPUT;
            return SSC_NAMED;
        case SSC_REQINPUT:
            if (input) {
                user->status = SSC_NAMED;
            }
            return SSC_REQINPUT;
        default:
            break;
    }
    return user->status;
}

int user_input_handler(chatroom_user *user, char *input) {
    if (user_stat_handler(user, input) & (SSC_REQNAME | SSC_EXECING)) return 0;
    char *neat_input = input_filter(input);

    if (neat_input == NULL) return 0;

    char *dup_input = strdup(neat_input);
    char *split = cmdtok(dup_input, "|");
    for (; split; split = cmdtok(NULL, "|")) {
        char *name = strtok(split, " ");
        char *param = strtok(NULL, "");

        cmd_element *cmd_addr;
        if ((cmd_addr = check_cmd(name)) == NULL) {
            dprintf(user->fd->write, "command not found: \"%s\" doesn't exit\n",
                    name);
            free(split);
            break;
        }

        waiting_cmd wait_cmd;
        init_waitingcmd(&wait_cmd, cmd_addr, user, param);

        if (append_queue(wait_cmd) == -1) {
            free_all_waiting_cmd();
            return -1;
        }
        free(split);
    }

    /*
     * God, please forgive me. Here is the most ugly code in this program.
     * START of UGLY CODE
     */
    waiting_cmd *first_cmd = get_n_waiting_cmd(0);
    if (first_cmd && strcmp(first_cmd->cmd_addr->name, "name") == 0) {
        do_name(*first_cmd->cmd_addr, first_cmd->param,
                first_cmd->additional_data);
        free_all_waiting_cmd();
        free(neat_input);
        free(dup_input);
        return 0;
    }
    /* END of UGLY CODE */

    pid_t child = fork();
    if (child == 0) {
        /* child process */
        if (user->fd->read) dup2(user->fd->read, STDIN_FILENO);
        if (user->fd->write) dup2(user->fd->write, STDOUT_FILENO);
        if (exec_all_waiting_cmd() == -1) {
            exit(EXIT_FAILURE);
        }
        while (wait(NULL) != -1)
            ;
        exit(EXIT_SUCCESS);
    }
    /* parent process */
    free_all_waiting_cmd();
    free(neat_input);
    free(dup_input);

    user->status = SSC_EXECING;
    user->console = child;
    return 0;
}

int server_start() {
    struct __ipv4_server server;
    char *ip = "172.22.46.36";
    int port = 4321;
    int socket_fd = ipv4_config(&server, inet_addr(ip), htons(port), 1);
    int fd[2] = {socket_fd, socket_fd};
    add_pfd(fd, SSC_SOCK_SERV);
    printf("server info: %s %d\n", ip, port);

    

    while (1) {
        socklen_t len = sizeof(struct sockaddr_in);
        chatroom_user *new_user =
            register_ipv4conn(socket_fd, &server, &len, SOCK_NONBLOCK);

        if (user_list == NULL) continue;
        chatroom_user *tmp = user_list;
        do {
            /* read from every user socket fd */
            char input[1024] = {0};
            int length = read(tmp->fd->read, input, sizeof(input) - 1);

            switch (length) {
                case -1:
                    if (errno == EAGAIN) {
                        user_stat_handler(tmp, NULL);
                        break;
                    }
                    perror("read() error");
                    continue;
                    break;
                case 0:
                    tmp = close_user(tmp);
                    continue;
                default:
                    user_input_handler(tmp, input);
                    break;
            }

            if (tmp->status & SSC_EXECING) {
                pid_t p = waitpid(tmp->console, NULL, WNOHANG);
                if (p > 0) {
                    tmp->status = SSC_NAMED;
                }
            }
            if (tmp) tmp = tmp->next;
        } while (tmp && tmp != user_list);
    }
}

int do_who(struct __cmd_element who, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    printf(" %-15s%-15s\n", "<name>", "<IP:port>");
    chatroom_user *tmp = user_list;
    do {
        if (tmp == self)
            printf("*");
        else
            printf(" ");

        struct sockaddr_in fd_info;
        socklen_t fd_size = sizeof(fd_info);
        getsockname(tmp->fd->read, (struct sockaddr *)&fd_info, &fd_size);
        char *addr = inet_ntoa(fd_info.sin_addr);
        printf("%-15s%s:%d\n", tmp->name, addr, fd_info.sin_port);

        tmp = tmp->next;
    } while (tmp != user_list);

    return 0;
}

int do_tell(struct __cmd_element tell, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    char *name = strtok(params, " ");
    char *msg = strtok(NULL, "");
    if (name == NULL) {
        printf("who are you telling?\n");
        return 0;
    }
    if (msg == NULL) {
        printf("what are you telling?\n");
    }

    chatroom_user *tmp = user_list;
    do {
        if (strcmp(tmp->name, name) == 0) {
            dprintf(tmp->fd->write, "<user:%-10s told you>: %s\n", self->name,
                    msg);
        }
        tmp = tmp->next;
    } while (tmp != user_list);

    return 0;
}

int do_yell(struct __cmd_element yell, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    char *msg = params;

    if (msg == NULL) {
        printf("what are you yelling?\n");
    }

    chatroom_user *tmp = user_list;
    do {
        dprintf(tmp->fd->write, "<user:%-10s yelled>: %s\n", self->name, msg);
        tmp = tmp->next;
    } while (tmp != user_list);

    return 0;
}

int do_name(struct __cmd_element name, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    char *new_name = params;
    if (name_exist_in_system(new_name)) {
        dprintf(self->fd->write, "User name exist, Please change\n");
        return 0;
    }
    strncpy(self->name, new_name, 1024);
    return 0;
}


int do_listMail(struct __cmd_element who, char *params, ...);
int do_sentMail(struct __cmd_element who, char *params, ...);
int do_delMail(struct __cmd_element who, char *params, ...);

int do_Groups(struct __cmd_element who, char *params, ...);
int do_gyell(struct __cmd_element who, char *params, ...);
int do_listGroup(struct __cmd_element who, char *params, ...);
int do_createGroup(struct __cmd_element who, char *params, ...);
int do_delGroup(struct __cmd_element who, char *params, ...);
int do_addGroup(struct __cmd_element who, char *params, ...);
int do_leaveGroup(struct __cmd_element who, char *params, ...);
int do_kickUser(struct __cmd_element who, char *params, ...);

void name_handler() {}

int do_server(struct __cmd_element server, char *params, ...) {
    char *new_params = strdup(params);
    char **params_list = parse_params(new_params, 1);
    params_list[0] = server.name;

    free_all_waiting_cmd();
    if (add_builtin_command("who", NULL, do_who) == -1) return -1;
    if (add_builtin_command("tell", NULL, do_tell) == -1) return -1;
    if (add_builtin_command("yell", NULL, do_yell) == -1) return -1;
    if (add_builtin_command("name", NULL, do_name) == -1) return -1;

    // if (add_builtin_command("listMail", NULL, do_listMail) == -1) return -1;
    // if (add_builtin_command("sentMail", NULL, do_sentMail) == -1) return -1;
    // if (add_builtin_command("delMail", NULL, do_delMail) == -1) return -1;

    // if (add_builtin_command("Groups", NULL, do_Groups) == -1) return -1;
    // if (add_builtin_command("gyell", NULL, do_gyell) == -1) return -1;
    // if (add_builtin_command("listGroup", NULL, do_listGroup) == -1) return -1;
    // if (add_builtin_command("createGroup", NULL, do_createGroup) == -1) return -1;
    // if (add_builtin_command("delGroup", NULL, do_delGroup) == -1) return -1;
    // if (add_builtin_command("addGroup", NULL, do_addGroup) == -1) return -1;
    // if (add_builtin_command("leaveGroup", NULL, do_leaveGroup) == -1) return -1;
    // if (add_builtin_command("kickUser", NULL, do_kickUser) == -1) return -1;

    signal(SIGUSR1, name_handler);

    int success = 0;
    if (strcmp(params_list[1], "start") == 0) {
        printf("Server start O.<\n");
        printf("Connecting to redis server :)\n");

        success = server_start();
        exit(!(!success));
    } else {
        printf("Parameter not found\n");
    }

    exit(EXIT_SUCCESS);
}

/* debug */

void showall_user() {
    chatroom_user *tmp = user_list;
    do {
        printf("%s <--> ", tmp->name);
        tmp = tmp->next;
    } while (tmp != user_list);
    printf("\n");
}

void show_inputstring(char *str) {
    for (char *c = str; c && *c; c++) {
        printf("%d ", *c);
    }
    printf("\n");
}