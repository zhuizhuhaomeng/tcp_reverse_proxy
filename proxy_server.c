/**
 *
 * Usage
 *
 * make
 * ./sproxy_server -s 127.0.0.1:6677 -t 127.0.0.1:6379 -d
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include <locale.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/wait.h>

#define max(m,n) ((m) > (n) ? (m) : (n))
#define SLOG(fmt, ...) message(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

void message(const char *filename, int line, const char *fmt, ...)
{
    char sbuf[1024], tbuf[30];
    va_list args;
    time_t now;
    uint len;

    va_start(args, fmt);
    len = vsnprintf(sbuf, sizeof(sbuf), fmt, args);
    va_end(args);

    if (len >= sizeof(sbuf)) {
        memcpy(sbuf + sizeof(sbuf) - sizeof("..."), "...", sizeof("...") - 1);
        len = sizeof(sbuf) - 1;
    }
    sbuf[len] = '\0';

    now = time(NULL);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %X", localtime(&now));
    fprintf(stderr, "%s|%u|%s,%d|%s\n", tbuf, getpid(), filename, line, sbuf);
}

typedef struct {
    char proxy_server[16];
    int server_port;
    char proxy_client[16];
    int client_port;
    int running;
    int daemon;
    pid_t master_pid;
} ProxyServer;

ProxyServer ps = { "127.0.0.1", 7777, "127.0.0.1", 8888, 1, 0, 0 };
int accept_server_fd;
int accept_client_fd;

void signal_handle(int sig)
{
    int stat;
    if (sig == SIGCHLD) {
        wait(&stat);
    } else {
        ps.running = 0;
        if (ps.master_pid) {
            kill(-(ps.master_pid), 9);
        }
    }
}

void show_usage(const char *name)
{
    fprintf(stderr, "Usage like %s -s 127.0.0.1:7777 -t 127.0.0.1:8888 -d\n", name);
}


void SetNoblock(int fd)
{
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) == -1) {
        SLOG("set %d nonblocking failed", fd);
    }
}

void EnableReuseAddr(int fd)
{
    int enable = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        SLOG("setsockopt(SO_REUSEADDR) failed");
    }
}

int listen_server(const char *host, int port)
{
    struct sockaddr_in serv_addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        SLOG("connect server %s:%d failed", host, port);
        return fd;
    }
    EnableReuseAddr(fd);

    memset(&serv_addr, '0', sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0) {
        SLOG("connect server %s:%d failed", host, port);
        return fd;
    }

    if (bind(fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1) {
        SLOG("connect server %s:%d failed", host, port);
        return fd;
    }

    if (listen(fd, 5) != 0) {
        SLOG("set listen server %s:%d failed", host, port);
        close(fd);
        return -1;
    }

    SLOG("connect server %s:%d success", host, port);

    return fd;
}


int sendAll(int fd, char *buf, int n)
{
    int ret;
    fd_set writefds, exceptfds;

    int m = 0;
    while (1) {
        ret = write(fd, buf + m, n - m);
        if (ret <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                FD_ZERO(&writefds);
                FD_ZERO(&exceptfds);

                FD_SET(fd, &writefds);
                FD_SET(fd, &exceptfds);
                ret = select(fd + 1, NULL, &writefds, &exceptfds, NULL);
            } else {
                return -1;
            }
            continue;
        }
        m += ret;
        if (m == n) {
            break;
        }
    }
    return 0;
}

void child_process(int client_fd)
{
    SLOG("forked child start|%d", client_fd);

    int server_fd = -1;
    int nfds = -1;
    int ready = -1;
    int n = 0;

    struct sockaddr_in client_addr;
    socklen_t addr_len;

    server_fd = accept(accept_server_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (server_fd < 0) {
        goto endprocess;
    }
    EnableReuseAddr(server_fd);
    SLOG("sproxy server accept server fd %d", client_fd);
    SetNoblock(client_fd);
    SetNoblock(server_fd);

    fd_set readfds, exceptfds;
    char buf[1024];
    int start_flag = 0x11223344;
    n = write(client_fd, &start_flag, sizeof(start_flag));
    while (ps.running) {
        FD_ZERO(&readfds);
        FD_ZERO(&exceptfds);

        FD_SET(client_fd, &readfds);
        FD_SET(client_fd, &exceptfds);
        FD_SET(server_fd, &readfds);
        FD_SET(server_fd, &exceptfds);

        nfds = max(server_fd, client_fd);
        ready = select(nfds + 1, &readfds, NULL, &exceptfds, NULL);
        if (ready < 0
            && errno == EINTR) {
            continue;
        }

        if (ready <= 0) {
            SLOG("select read event failed");
            continue;
        }

        if (FD_ISSET(client_fd, &exceptfds)) {
            goto endprocess;
        }

        if (FD_ISSET(client_fd, &readfds)) {
            SLOG("trigger proxy read");
            while ((n = read(client_fd, buf, sizeof(buf))) > 0) {
                if (sendAll(server_fd, buf, n) != 0) {
                    goto endprocess;
                }
            }

            if (n == 0) {
                goto endprocess;
            }
        }

        if (FD_ISSET(server_fd, &readfds)) {
            while ((n = read(server_fd, buf, sizeof(buf))) > 0) {
                if (sendAll(client_fd, buf, n) != 0) {
                    goto endprocess;
                }
            }

            if (n == 0) {
                goto endprocess;
            }
        }
    }

endprocess : {
        if (client_fd > 0) {
            close(client_fd);
        }

        if (server_fd > 0) {
            close(server_fd);
        }
    }

    SLOG("child_process end");
}


int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    struct sockaddr_in client_addr;
    socklen_t addr_len;

    if (argc == 1) {
        show_usage((const char *)argv[0]);
        return 1;
    }

    int c;
    while ((c = getopt(argc, argv, "dhs:t:")) != -1) {
        switch (c) {
        case 's':
            if (sscanf(optarg, "%[0-9.]:%d", ps.proxy_server, &ps.server_port) != 2) {
                show_usage((const char *)argv[0]);
                return 1;
            }
            break;
        case 't':
            if (sscanf(optarg, "%[0-9.]:%d", ps.proxy_client, &ps.client_port) != 2) {
                show_usage((const char *)argv[0]);
                return 1;
            }
            break;
        case 'd':
            ps.daemon = 1;
            break;

        default:
            printf("cccc = %c\n", c);
            show_usage((const char *)argv[0]);
            return 1;
        }
    }

    SLOG("sproxy server %s:%d  client %s:%d", ps.proxy_server, ps.server_port, ps.proxy_client, ps.client_port);

    signal(SIGINT,  signal_handle);
    signal(SIGTERM, signal_handle);
    signal(SIGCHLD, signal_handle);

    if (ps.daemon) {
        switch (fork()) {
        case -1:
            break;
        case 0:
            break;
        default:
            _exit(0);
        }

        setsid();
        umask(0);
        close(0);
        close(1);
        close(2);
    }

    ps.running = 1;
    ps.master_pid = getpid();

    pid_t forked_pid = -1;
    accept_server_fd = listen_server(ps.proxy_server, ps.server_port);
    accept_client_fd = listen_server(ps.proxy_client, ps.client_port);
    SLOG("%s:%d accept server fd:%d", ps.proxy_server, ps.server_port, accept_server_fd);
    SLOG("%s:%d accept client fd:%d", ps.proxy_client, ps.client_port, accept_client_fd);

    if (accept_client_fd == -1 || accept_server_fd == -1) {
        SLOG("listen failed");
        return -1;
    }
    while (1) {
        int client_fd = accept(accept_client_fd, (struct sockaddr *)&client_addr, &addr_len);
        SLOG("sproxy server accept client fd %d", client_fd);
        if (client_fd == -1) {
            continue;
        }
        EnableReuseAddr(client_fd);
        forked_pid = fork();
        if (forked_pid == 0) {
            child_process(client_fd);
            exit(0);
        } else {
            close(client_fd);
        }
    }

    SLOG("parent_exit|%u", getpid());

    return 0;
}
