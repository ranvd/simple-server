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
#include <time.h>
#include <unistd.h>

#define _GNU_SOURCE
#include <sys/socket.h>

#include "console.h"
#include "hiredis.h"
#include "read.h"
#include "server.h"
#include "utils.h"

#ifndef EXIT_IF_FAIL
#define EXIT_IF_FAIL(v, fail, err_info) \
    if (v == fail) {                    \
        perror(err_info);               \
        exit(EXIT_FAILURE);             \
    }
#endif

#define SSC_NONAME 0     /* not named (not send request yet)*/
#define SSC_REQNAME 1    /* not named (sent request)*/
#define SSC_NAMED 2      /* named user & free to input command */
#define SSC_REQINPUT 4   /* send chat> already */
#define SSC_EXECING 8    /* user's input under executing */
#define SSC_REQPASSWD 16 /* request user input password */

int do_name(struct __cmd_element name, char *params, ...);
int add_user_to_group(char *group, char *name, int prior);

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

redisContext *redisdb = NULL;
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

int group_exist_in_system(char *group) {
    if (group == NULL) return 1;

    int rtv;
    redisReply *reply =
        redisCommand(redisdb, "SISMEMBER Chatroom.group %s", group);
    rtv = reply->integer;
    freeReplyObject(reply);

    return rtv;
}

int name_exist_in_system(char *name) {
    if (name == NULL) return 0;

    int rtv;
    redisReply *reply = redisCommand(redisdb, "SISMEMBER Chatroom %s", name);
    rtv = reply->integer;
    freeReplyObject(reply);

    return rtv;
    // chatroom_user *tmp = user_list;
    // do {
    //     if (strcmp(name, tmp->name) == 0) return 1;
    //     tmp = tmp->next;
    // } while (tmp != user_list);
    // return 0;
}

int init_waitingcmd(waiting_cmd *w_cmd, cmd_element *cmd, chatroom_user *user,
                    char *args) {
    w_cmd->read = w_cmd->write = NULL;
    w_cmd->cmd_addr = cmd;
    if (strcmp(cmd->name, "who") == 0 || strcmp(cmd->name, "tell") == 0 ||
        strcmp(cmd->name, "yell") == 0 || strcmp(cmd->name, "name") == 0 ||
        strcmp(cmd->name, "listMail") == 0 ||
        strcmp(cmd->name, "sentMail") == 0 ||
        strcmp(cmd->name, "delMail") == 0 || strcmp(cmd->name, "Groups") == 0 ||
        strcmp(cmd->name, "gyell") == 0 ||
        strcmp(cmd->name, "listGroup") == 0 ||
        strcmp(cmd->name, "createGroup") == 0 ||
        strcmp(cmd->name, "delGroup") == 0 ||
        strcmp(cmd->name, "addGroup") == 0 ||
        strcmp(cmd->name, "leaveGroup") == 0 ||
        strcmp(cmd->name, "kickUser") == 0) {
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

int register_user(char *name) {
    /* TODO: */
    redisReply *reply;
    reply = redisCommand(redisdb, "SADD Chatroom %s", name);
    freeReplyObject(reply);
    return 0;
}
int check_passwd(char *name, char *passwd) {
    if (name == NULL || passwd == NULL) return -1;

    int rtv = 0;
    redisReply *reply;
    reply = redisCommand(redisdb, "GET %s", name);
    if (reply->type == REDIS_REPLY_NIL) {
        reply = redisCommand(redisdb, "SET %s %s", name, passwd);
        rtv = (reply->len == 2) ? 1 : 0;
        freeReplyObject(reply);
        return rtv;
    }
    rtv = (strcmp(reply->str, passwd) == 0) ? 1 : 0;
    freeReplyObject(reply);
    return rtv;
    /* TODO: if the db return nil, than set passwd as password. */
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

                if (!name_exist_in_system(neat_name)) {
                    register_user(neat_name);
                }
                strncpy(user->name, neat_name, 1024);
                user->status = SSC_REQPASSWD;
                dprintf(user->fd->write, "Password: ");

                free(neat_name);
            }
            return SSC_REQNAME;
            break;
        case SSC_REQPASSWD:
            if (input) {
                char *neat_passwd = input_filter(input);

                if (check_passwd(user->name, neat_passwd) == 1) {
                    /* Success */
                    dprintf(user->fd->write, "Welcome %s!\n", user->name);
                    user->status = SSC_NAMED;

                    redisReply *reply = redisCommand(
                        redisdb, "SADD Chatroom.online %s", user->name);
                    freeReplyObject(reply);
                } else {
                    dprintf(user->fd->write, "Password: ");
                }
                free(neat_passwd);
            }
            return SSC_REQPASSWD;
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
    if (user_stat_handler(user, input) &
        (SSC_REQNAME | SSC_EXECING | SSC_REQPASSWD))
        return 0;
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
                    redisReply *reply = redisCommand(
                        redisdb, "SREM Chatroom.online %s", tmp->name);
                    freeReplyObject(reply);
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
    redisFree(redisdb);
    return 0;
}

int do_who(struct __cmd_element who, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    printf(" %-15s%-15s\n", "<name>", "<IP:port>");
    printf(GREEN_LIGHT);
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
        printf("%-15s%-15s:%d\n", tmp->name, addr, fd_info.sin_port);

        tmp = tmp->next;
    } while (tmp != user_list);

    printf(RESET_LIGHT);

    redisReply *reply;
    reply = redisCommand(redisdb, "SDIFF Chatroom Chatroom.online");
    for (int i = 0; i < reply->elements; i++) {
        char *name = reply->element[i]->str;
        printf(" %-15s%-15s:%d\n", name, "offline", -1);
    }
    freeReplyObject(reply);
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
        return 0;
    }

    int foo = 1;
    chatroom_user *tmp = user_list;
    do {
        if (strcmp(tmp->name, name) == 0) {
            dprintf(tmp->fd->write, "<user:%-10s told you>: %s\n", self->name,
                    msg);
            foo = 0;
            break;
        }
        tmp = tmp->next;
    } while (tmp != user_list);

    if (foo) printf("%s is offline, try again later\n", name);
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

int del_user_from_group(char *group, char *user) {
    int rtv;
    redisReply *gpmem, *userlist;

    gpmem = redisCommand(redisdb, "ZREM %s %s", group, user);
    rtv = gpmem->integer;

    userlist = redisCommand(redisdb, "LREM %s.group 0 %s", user, group);

    freeReplyObject(gpmem);
    freeReplyObject(userlist);
    return !(!rtv);
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

    register_user(new_name);

    char *old_name = strdup(self->name);
    strncpy(self->name, new_name, 1024);

    redisReply *gps, *add, *del0, *del1, *del2;
    gps = redisCommand(redisdb, "LRANGE %s.group 0 -1", old_name);
    for (int i = 0; i < gps->elements; i++) {
        char *gp = gps->element[i]->str;
        redisReply *check;
        check = redisCommand(redisdb, "ZRANGE %s 0 0", gp);
        if (strcmp(check->element[0]->str, old_name) == 0) {
            add_user_to_group(gp, new_name, 0);
        } else {
            add_user_to_group(gp, new_name, 10);
        }
        del_user_from_group(gp, old_name);
    }
    add = redisCommand(redisdb, "SADD Chatroom.online %s", new_name);
    del0 = redisCommand(redisdb, "SREM Chatroom.online %s", old_name);
    del1 = redisCommand(redisdb, "SREM Chatroom %s", old_name);
    del2 = redisCommand(redisdb, "DEL %s %s.group %s.mail", old_name, old_name, old_name);

    freeReplyObject(add);
    freeReplyObject(del0);
    freeReplyObject(del1);
    freeReplyObject(del2);
    freeReplyObject(gps);

    return 0;
}

int do_listMail(struct __cmd_element who, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    printf("<id> <date>             <sender>        <message>\n");
    redisReply *reply =
        redisCommand(redisdb, "LRANGE %s.mail 0 -1", self->name);
    for (int i = 0; i < reply->elements; i += 4) {
        char *date = reply->element[i]->str;
        char *time = reply->element[i + 1]->str;
        char *author = reply->element[i + 2]->str;
        char *msg = reply->element[i + 3]->str;
        printf("%4d %-10s %-8s %-15s %-25s\n", i / 4, date, time, author, msg);
    }
    return 0;
}

int do_sentMail(struct __cmd_element who, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    char *name = strtok(params, " ");
    char *msg = strtok(NULL, "");
    if (name == NULL) {
        printf("who do you want to sent?\n");
        return 0;
    }
    if (msg == NULL) {
        printf("what msg. do you want to sent?\n");
        return 0;
    }

    if (name_exist_in_system(name)) {
        redisReply *reply;
        time_t t = time(NULL);
        struct tm tm = *localtime(&t);
        reply = redisCommand(redisdb,
                             "rpush %s.mail %d-%02d-%02d %02d:%02d:%02d %s %s",
                             name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                             tm.tm_hour, tm.tm_min, tm.tm_sec, self->name, msg);

        freeReplyObject(reply);
    } else {
        printf("%s%s doesn't exist in database\n%s", RED_LIGHT, name,
               RESET_LIGHT);
    }
    return 0;
}

int do_delMail(struct __cmd_element who, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    char *idx_str = strtok(params, " ");
    long int idx = strtol(idx_str, idx_str + 10, 10);

    redisReply *del, *rem;
    for (int i = idx * 4; i < idx * 4 + 4; i++) {
        del = redisCommand(redisdb, "LSET %s.mail %d /SD_DELETE_ED/",
                           self->name, i);
        freeReplyObject(del);
    }
    rem = redisCommand(redisdb, "LREM %s.mail 4 /SD_DELETE_ED/", self->name);
    freeReplyObject(rem);

    return 0;
}

int add_user_to_group(char *group, char *name, int prior) {
    int rtv;
    redisReply *addgroup, *addlist;
    addgroup = redisCommand(redisdb, "ZADD %s %d %s", group, prior, name);

    rtv = addgroup->integer;
    if (rtv) {
        addlist = redisCommand(redisdb, "RPUSH %s.group %s", name, group);
        freeReplyObject(addlist);
    }
    freeReplyObject(addgroup);

    return rtv;
}

int do_Groups(struct __cmd_element who, char *params, ...) {
    printf("The groups in system: \n");
    redisReply *reply = redisCommand(redisdb, "SMEMBERS Chatroom.group");
    for (int i = 0; i < reply->elements; i++) {
        printf("%d) %s\n", i, reply->element[i]->str);
    }
    return 0;
}

int do_gyell(struct __cmd_element who, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    char *gpname = strtok(params, " ");
    char *msg = strtok(NULL, "");

    redisReply *check;
    check = redisCommand(redisdb, "ZRANK %s %s", gpname, self->name);
    if (check->type == REDIS_REPLY_NIL) {
        printf("%sShut up, you are not the member: %s\n%s", RED_LIGHT, gpname, RESET_LIGHT);
        freeReplyObject(check);
        return -1;
    }
    freeReplyObject(check);

    redisReply *gpmem;
    gpmem = redisCommand(redisdb, "ZRANGE %s 0 -1", gpname);

    for (int i = 0; i < gpmem->elements; i++) {
        char *mem = gpmem->element[i]->str;
        char *args = calloc(strlen(mem) + strlen(msg) + 2, sizeof(char));
        strcat(args, mem);
        args[strlen(mem)] = ' ';
        strcat(args, msg);
        do_tell(who, args, self);
    }

    freeReplyObject(gpmem);
    return 0;
}

int do_listGroup(struct __cmd_element who, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    redisReply *reply =
        redisCommand(redisdb, "LRANGE %s.group 0 -1", self->name);
    printf("Groups: \n");
    for (int i = 0; i < reply->elements; i++) {
        printf("%d) %s\n", i, reply->element[i]->str);
    }
    freeReplyObject(reply);
    return 0;
}

int do_createGroup(struct __cmd_element who, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    char *gpname = strtok(params, " ");
    if (group_exist_in_system(gpname)) {
        printf("%sGroup name exist\n%s", RED_LIGHT, RESET_LIGHT);
        return -1;
    }

    redisReply *reply = redisCommand(redisdb, "SADD Chatroom.group %s", gpname);
    add_user_to_group(gpname, self->name, 0);
    freeReplyObject(reply);
    printf("Created Successfully\n");

    return 0;
}

int do_delGroup(struct __cmd_element who, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    char *gpname = strtok(params, " ");
    if (!group_exist_in_system(gpname)) {
        printf("%sGroup name not exist\n%s", RED_LIGHT, RESET_LIGHT);
        return -1;
    }

    redisReply *reply = redisCommand(redisdb, "ZRANGE %s 0 0", gpname);
    if (strcmp(reply->element[0]->str, self->name) == 0) {
        redisReply *rm, *rm2, *del, *u;
        rm = redisCommand(redisdb, "SREM Chatroom.group %s", gpname);

        u = redisCommand(redisdb, "ZRANGE %s 0 -1", gpname);
        for (int i = 0; i < u->elements; i++) {
            char *user = u->element[i]->str;
            rm2 = redisCommand(redisdb, "LREM %s.group 0 %s", user, gpname);
            freeReplyObject(rm2);
        }
        del = redisCommand(redisdb, "DEL %s", gpname);
        freeReplyObject(rm);
        freeReplyObject(del);
        freeReplyObject(u);
    } else {
        printf("%sYou're not allow to delete this group\n%s", RED_LIGHT,
               RESET_LIGHT);
    }
    freeReplyObject(reply);

    return 0;
}

int do_addGroup(struct __cmd_element who, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    char *gpname = strtok(params, " ");
    if (!group_exist_in_system(gpname)) {
        printf("%sGroup name not exist\n%s", RED_LIGHT, RESET_LIGHT);
        return -1;
    }

    if (add_user_to_group(gpname, self->name, 10)) {
        printf("Join Successfully\n");
    } else {
        printf("Join Failed\n");
    }

    return 0;
}

int do_leaveGroup(struct __cmd_element who, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    char *gpname = strtok(params, " ");

    redisReply *check;
    check = redisCommand(redisdb, "ZRANK %s %s", gpname, self->name);
    if (check->type == REDIS_REPLY_NIL) {
        printf("%sYou are not in this group\n%s", RED_LIGHT, RESET_LIGHT);
        return 0;
    }

    redisReply *gpowner, *rmgroup, *rmlist;
    gpowner = redisCommand(redisdb, "ZRANGE %s 0 1", gpname);
    if (gpowner->elements < 2) {
        printf("delete Group...\n");
        return do_delGroup(who, gpname, self);
    }

    if (gpowner->elements == 2 &&
        strcmp(gpowner->element[0]->str, self->name) == 0) {
        char *nxt_owner = gpowner->element[1]->str;
        printf("Change user from %s to %s\n", self->name, nxt_owner);
        redisReply *chg_prior;
        chg_prior =
            redisCommand(redisdb, "ZADD %s %d %s", gpname, 0, nxt_owner);
        freeReplyObject(chg_prior);
    }

    rmgroup = redisCommand(redisdb, "ZREM %s %s", gpname, self->name);
    rmlist = redisCommand(redisdb, "LREM %s.group 0 %s", self->name, gpname);

    return 0;
}

int do_kickUser(struct __cmd_element who, char *params, ...) {
    va_list ap;
    va_start(ap, params);
    chatroom_user *self = va_arg(ap, chatroom_user *);
    va_end(ap);

    char *gpname = strtok(params, " ");

    if (!group_exist_in_system(gpname)) {
        printf("%sGroup name not exist\n%s", RED_LIGHT, RESET_LIGHT);
        return -1;
    }

    redisReply *check;
    check = redisCommand(redisdb, "ZRANGE %s 0 0", gpname);
    if (strcmp(check->element[0]->str, self->name) != 0) {
        printf("%sYou're not allow to kick others\n%s", RED_LIGHT, RESET_LIGHT);
        return -1;
    }

    char *user = strtok(NULL, " ");
    while (user) {
        if (del_user_from_group(gpname, user)) {
            printf("Delete success: %s\n", user);
        } else {
            printf("%sUser not found: %s\n%s", RED_LIGHT, user, RESET_LIGHT);
        }
        user = strtok(NULL, " ");
    }

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

    if (add_builtin_command("listMail", NULL, do_listMail) == -1) return -1;
    if (add_builtin_command("sentMail", NULL, do_sentMail) == -1) return -1;
    if (add_builtin_command("delMail", NULL, do_delMail) == -1) return -1;

    if (add_builtin_command("Groups", NULL, do_Groups) == -1) return -1;
    if (add_builtin_command("gyell", NULL, do_gyell) == -1) return -1;
    if (add_builtin_command("listGroup", NULL, do_listGroup) == -1) return -1;
    if (add_builtin_command("createGroup", NULL, do_createGroup) == -1)
        return -1;
    if (add_builtin_command("delGroup", NULL, do_delGroup) == -1) return -1;
    if (add_builtin_command("addGroup", NULL, do_addGroup) == -1) return -1;
    if (add_builtin_command("leaveGroup", NULL, do_leaveGroup) == -1) return -1;
    if (add_builtin_command("kickUser", NULL, do_kickUser) == -1) return -1;

    int success = 0;
    if (strcmp(params_list[1], "start") == 0) {
        printf("Server start\n");
        printf("Connecting to redis server :)\n");

        redisdb = redisConnect("127.0.0.1", 6379);
        if (redisdb == NULL || redisdb->err) {
            if (redisdb) {
                printf("Error: %s\n", redisdb->errstr);
            } else {
                printf("Can't allocate redis context\n");
            }
            exit(EXIT_FAILURE);
        }

        printf("Redis server Connected :)\n");
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