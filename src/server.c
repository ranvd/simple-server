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

#define C_USER_STATUS_I 0   /* not named (not send request yet)*/
#define C_USER_STATUS_II 1  /* not named (sent request)*/
#define C_USER_STATUS_III 2 /* named user */

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
    pfd_element *new_pfd = add_pfd(fd);

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
    if (start == output) return NULL;
    return output;
}

int name_exist_in_system(char *name) {
    chatroom_user *tmp = user_list;
    do {
        if (strcmp(name, tmp->name) == 0) return 1;
        tmp = tmp->next;
    } while (tmp != user_list);
    return 0;
}

int handle_user_input(chatroom_user *tmp, char *input) {
    char *neat_input = input_filter(input);

    if (tmp->status == C_USER_STATUS_II) {
        if (neat_input) {
            if (name_exist_in_system(neat_input)) {
                tmp->status = C_USER_STATUS_I;
                dprintf(tmp->fd->write, "User name exist, Please change\n");
            } else {
                strncpy(tmp->name, neat_input, 1024);
                tmp->status = C_USER_STATUS_III;
            }
        } else {
            tmp->status = C_USER_STATUS_I;
        }
        return 0;
    }

    if (neat_input == NULL) return 0;

    char *dup_input = strdup(neat_input);
    char **parse_list = parse_params(dup_input, 0);
    cmd_element *cmd;
    if ((cmd = check_cmd(parse_list[0])) == NULL) {
        ssize_t size = write(tmp->fd->write, "command not found\n",
                             sizeof("command not found\n"));
        if (size == -1) perror("In server/handle_user_input()");
        return 0;
    }

    cmd->operation(*cmd, neat_input, tmp);
    free(neat_input);
    free(dup_input);
    return 0;
}

int server_start() {
    struct __ipv4_server server;
    char *ip = "172.22.46.36";
    int port = 4321;
    int socket_fd = ipv4_config(&server, inet_addr(ip), htons(port), 1);
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
                        if (tmp->status == C_USER_STATUS_I) {
                            ssize_t size =
                                write(tmp->fd->write,
                                      "Who're you: ", sizeof("Who're you: "));
                            if (size == -1) {
                                perror("In server/server_start()");
                            }
                            tmp->status = C_USER_STATUS_II;
                        }
                        break;
                    }
                    perror("read() error");
                    continue;
                    break;
                case 0:
                    tmp = close_user(tmp);
                    continue;
                default:
                    handle_user_input(tmp, input);
                    ssize_t size =
                        write(tmp->fd->write, "chat> ", sizeof("chat> "));
                    if (size == -1) {
                        perror("In server/server_start()");
                    }
                    break;
            }
            if (tmp) tmp = tmp->next;
        } while (tmp && tmp != user_list);
    }
}

int do_who(struct __cmd_element server, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    dprintf(self->fd->write, " %-15s%-15s\n", "<name>", "<IP:port>");
    chatroom_user *tmp = user_list;
    do {
        if (tmp == self)
            dprintf(self->fd->write, "*");
        else
            dprintf(self->fd->write, " ");

        struct sockaddr_in fd_info;
        socklen_t fd_size = sizeof(fd_info);
        getsockname(tmp->fd->write, (struct sockaddr *)&fd_info, &fd_size);
        char *addr = inet_ntoa(fd_info.sin_addr);
        dprintf(self->fd->write, "%-15s%s:%d\n", tmp->name, addr,
                fd_info.sin_port);

        tmp = tmp->next;
    } while (tmp != user_list);

    return 0;
}

int do_tell(struct __cmd_element server, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    char *name = strtok(params, " ");
    name = strtok(NULL, " ");
    char *msg = strtok(NULL, "");
    if (name == NULL) {
        dprintf(self->fd->write, "who are you telling?\n");
        return 0;
    }
    if (msg == NULL) {
        dprintf(self->fd->write, "what are you telling?\n");
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

int do_yell(struct __cmd_element server, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    char *msg = strtok(params, " ");
    msg = strtok(NULL, "");

    if (msg == NULL) {
        dprintf(self->fd->write, "what are you yelling?\n");
    }

    chatroom_user *tmp = user_list;
    do {
        dprintf(tmp->fd->write, "<user:%-10s yelled>: %s\n", self->name, msg);
        tmp = tmp->next;
    } while (tmp != user_list);

    return 0;
}

int do_name(struct __cmd_element server, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    char *new_name = strtok(params, " ");
    new_name = strtok(NULL, "");

    if (name_exist_in_system(new_name)) {
        dprintf(self->fd->write, "User name exist, Please change\n");
        return 0;
    }
    strncpy(self->name, new_name, 1024);
    return 0;
}

int do_server(struct __cmd_element server, char *params, ...) {
    char *new_params = strdup(params);
    char **params_list = parse_params(new_params, 1);
    params_list[0] = server.name;

    free_all_waiting_cmd();
    if (add_builtin_command("who", NULL, do_who) == -1) return -1;
    if (add_builtin_command("tell", NULL, do_tell) == -1) return -1;
    if (add_builtin_command("yell", NULL, do_yell) == -1) return -1;
    if (add_builtin_command("name", NULL, do_name) == -1) return -1;

    int success = 0;
    if (strcmp(params_list[1], "start") == 0) {
        printf("server start\n");
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