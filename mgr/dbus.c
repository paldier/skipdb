#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/fcntl.h> // fcntl
#include <unistd.h> // close
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <time.h>
#include <limits.h> // [LONG|INT][MIN|MAX]
#include <errno.h>  // errno
#include <unistd.h>
#include "skipd.h"

typedef struct _dbclient {
    int remote_fd;
    char command[DELAY_KEY_LEN+1];
    char key[DELAY_KEY_LEN+1];
    time_t timeout;

    char* buf;
    int buf_max;
    int buf_len;
    int buf_pos;
} dbclient;

int create_client_fd(char* sock_path) {
    int len, remote_fd;
    struct sockaddr_un remote;

    if(-1 == (remote_fd = socket(AF_UNIX, SOCK_STREAM, 0))) {
        perror("socket");
        return -1;
    }

    remote.sun_family = AF_UNIX;
    strcpy(remote.sun_path, sock_path);
    len = strlen(remote.sun_path) + sizeof(remote.sun_family);
    if(-1 == connect(remote_fd, (struct sockaddr*)&remote, len)) {
        perror("connect");
        close(remote_fd);
        return -1;
    }

    return remote_fd;
}

int setnonblock(int fd) {
    int flags;

    flags = fcntl(fd, F_GETFL);
    flags |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}

static void check_buf(dbclient* client, int len) {
    int clen = _max(len, BUF_MAX);

    if((NULL != client->buf) && (client->buf_max < clen)) {
        free(client->buf);
        client->buf = NULL;
    }

    if(NULL == client->buf) {
        client->buf = (char*)malloc(clen+1);
        client->buf_max = clen;
    }
}

int read_util(dbclient* client, int len) {
    int clen, n;
    time_t now;

    check_buf(client, len);

    for(;;) {
        clen = len - client->buf_pos;
        n = recv(client->remote_fd, client->buf + client->buf_pos, clen, 0);
        if(n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                now = time(NULL);
                if(now > client->timeout) {
                    break;
                }

                usleep(50);
                continue;
            }
            //timeout
            return -2;
        } else if(n == 0) {
            //socket closed
            return -1;
        } else {
            client->buf_pos += n;
            if(client->buf_pos == len) {
                //read ok
                return 0;
            }
        }
    }

    //unkown error
    return -3;
}

int parse_common_result(dbclient *client) {
    int n1, n2;
    char* magic = MAGIC;

    do {
        client->timeout = time(NULL) + 110;

        client->buf_pos = 0;
        n1 = read_util(client, HEADER_PREFIX);
        if(n1 < 0) {
            return n1;
        }

        if(0 != memcmp(client->buf, magic, MAGIC_LEN)) {
            //message error
            return -3;
        }

        client->buf[HEADER_PREFIX-1] = '\0';
        if(S2ISUCCESS != str2int(&n2, client->buf+MAGIC_LEN, 10)) {
            //message error
            return -4;
        }

        client->buf_pos = 0;
        client->timeout = time(NULL) + 510;
        n1 = read_util(client, n2);
        if(n1 < 0) {
            return n1;
        }

        client->buf[n2] = '\0';
        printf("%s", client->buf);
    } while(0);

    return 0;
}

int parse_list_result(dbclient *client) {
    int n1, n2;
    char* magic = MAGIC;

    for(;;) {
        client->timeout = time(NULL) + 110;

        client->buf_pos = 0;
        n1 = read_util(client, HEADER_PREFIX);
        if(n1 < 0) {
            return n1;
        }

        if(0 != memcmp(client->buf, magic, MAGIC_LEN)) {
            //message error
            return -3;
        }

        client->buf[HEADER_PREFIX-1] = '\0';
        if(S2ISUCCESS != str2int(&n2, client->buf+MAGIC_LEN, 10)) {
            //message error
            return -4;
        }

        client->buf_pos = 0;
        client->timeout = time(NULL) + 510;
        n1 = read_util(client, n2);
        if(n1 < 0) {
            return n1;
        }

        client->buf[n2] = '\0';

        if(NULL != strstr(client->buf, "__end__")) {
            break;
        }
        printf("%s", client->buf);
    }

    return 0;
}

dbclient* gclient;
int main(int argc, char **argv)
{ 
    int n1, n2;
    dbclient* client;
    int remote_fd = create_client_fd("/tmp/.skipd_server_sock");
    if(-1 == remote_fd) {
        return -1;
    }
    gclient = (dbclient*)calloc(1, sizeof(dbclient));
    gclient->remote_fd = remote_fd;
    client = gclient;

    if(argc < 2) {
        return -11;
    }
    if(!strcmp("list", argv[1])) {
        if(argc < 3) {
            return -12;
        }
        strcpy(client->command, argv[1]);
        n1 = strlen(argv[2]) + 2 + strlen(client->command);
        check_buf(client, n1 + HEADER_PREFIX);
        n2 = snprintf(client->buf, client->buf_max, "%s%07d %s %s\n", MAGIC, n1, client->command, argv[2]);
        write(remote_fd, client->buf, n2);

        setnonblock(remote_fd);
        n1 = parse_list_result(gclient);
    } else if(!strcmp("get", argv[1])) {
        if(argc < 3) {
            return -12;
        }
        strcpy(client->command, argv[1]);
        n1 = strlen(argv[2]) + 2 + strlen(client->command);
        check_buf(client, n1 + HEADER_PREFIX);
        n2 = snprintf(client->buf, client->buf_max, "%s%07d %s %s\n", MAGIC, n1, client->command, argv[2]);
        write(remote_fd, client->buf, n2);

        setnonblock(remote_fd);
        n1 = parse_common_result(gclient);
    } else if(!strcmp("remove", argv[1])) {
        if(argc < 3) {
            return -12;
        }
        strcpy(client->command, argv[1]);
        n1 = strlen(argv[2]) + 2 + strlen(client->command);
        check_buf(client, n1 + HEADER_PREFIX);
        n2 = snprintf(client->buf, client->buf_max, "%s%07d %s %s\n", MAGIC, n1, client->command, argv[2]);
        write(remote_fd, client->buf, n2);

        setnonblock(remote_fd);
        n1 = parse_common_result(gclient);
    } else if(!strcmp("fire", argv[1])) {
        if(argc < 3) {
            return -12;
        }
        strcpy(client->command, argv[1]);
        n1 = strlen(argv[2]) + 2 + strlen(client->command);
        check_buf(client, n1 + HEADER_PREFIX);
        n2 = snprintf(client->buf, client->buf_max, "%s%07d %s %s\n", MAGIC, n1, client->command, argv[2]);
        write(remote_fd, client->buf, n2);

        setnonblock(remote_fd);
        n1 = parse_common_result(gclient);
    } else if(!strcmp("set", argv[1])) {
        if(argc < 4) {
            return -13;
        }
        strcpy(client->command, argv[1]);
        n1 = strlen(client->command) + strlen(argv[2]) + strlen(argv[3]) + 3;
        check_buf(client, n1 + HEADER_PREFIX);
        n2 = snprintf(client->buf, client->buf_max, "%s%07d %s %s %s\n", MAGIC, n1, client->command, argv[2], argv[3]);
        write(remote_fd, client->buf, n2);

        setnonblock(remote_fd);
        n1 = parse_common_result(client);
    } else if(!strcmp("ram", argv[1])) {
        if(argc < 4) {
            return -13;
        }
        strcpy(client->command, argv[1]);
        n1 = strlen(client->command) + strlen(argv[2]) + strlen(argv[3]) + 3;
        check_buf(client, n1 + HEADER_PREFIX);
        n2 = snprintf(client->buf, client->buf_max, "%s%07d %s %s %s\n", MAGIC, n1, client->command, argv[2], argv[3]);
        write(remote_fd, client->buf, n2);

        setnonblock(remote_fd);
        n1 = parse_common_result(client);
    } else if(!strcmp("replace", argv[1])) {
        if(argc < 4) {
            return -13;
        }
        strcpy(client->command, argv[1]);
        n1 = strlen(client->command) + strlen(argv[2]) + strlen(argv[3]) + 3;
        check_buf(client, n1 + HEADER_PREFIX);
        n2 = snprintf(client->buf, client->buf_max, "%s%07d %s %s %s\n", MAGIC, n1, client->command, argv[2], argv[3]);
        write(remote_fd, client->buf, n2);

        setnonblock(remote_fd);
        n1 = parse_common_result(client);
    } else if(!strcmp("event", argv[1])) {
        strcpy(client->command, "set");
        n1 = strlen(client->command) + strlen(argv[2]) + strlen(argv[3]) + 3 + 9;
        check_buf(client, n1 + HEADER_PREFIX);
        n2 = snprintf(client->buf, client->buf_max, "%s%07d %s __event__%s %s\n", MAGIC, n1, client->command, argv[2], argv[3]);
        write(remote_fd, client->buf, n2);

        setnonblock(remote_fd);
        n1 = parse_common_result(client);
    } 

    close(remote_fd);
    if(NULL != gclient->buf) {
        free(gclient->buf);
    }
    return 0;
}

