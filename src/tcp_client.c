#define _POSIX_C_SOURCE 200809L

#include "tcp.h"
#include "warehouse.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct
{
    const char *ip;
    int port;
    int id;
} Options;

static int read_options(Options *options, int argc, char **argv, bool robot)
{
    bool has_id = false;
    for (int index = 1; index < argc; ++index)
    {
        const char *name = argv[index];
        if (strcmp(name, "--help") == 0)
        {
            dprintf(STDOUT_FILENO, "Usage: %s [--ip IPV4] [--port PORT]%s\n",
                    argv[0], robot ? " --id ID" : "");
            return 1;
        }
        if (++index == argc)
        {
            dprintf(STDERR_FILENO, "Missing value for %s\n", name);
            return -1;
        }
        const char *value = argv[index];
        if (strcmp(name, "--ip") == 0)
        {
            options->ip = value;
        }
        else if (strcmp(name, "--port") == 0 &&
                 wh_tcp_int(value, 1, 65535, &options->port))
        {
            continue;
        }
        else if (robot && strcmp(name, "--id") == 0 &&
                 wh_tcp_int(value, 0, INT_MAX, &options->id))
        {
            has_id = true;
        }
        else
        {
            dprintf(STDERR_FILENO, "Invalid option: %s %s\n", name, value);
            return -1;
        }
    }
    if (robot && !has_id)
    {
        dprintf(STDERR_FILENO, "Robot id is required (--id ID)\n");
        return -1;
    }
    return 0;
}

static bool send_line(int fd, const char *line)
{
    size_t remaining = strlen(line);
    while (remaining > 0 && !wh_signal_stop)
    {
        ssize_t sent = send(fd, line, remaining, 0);
        if (sent > 0)
        {
            line += sent;
            remaining -= (size_t)sent;
        }
        else if (sent == 0 || errno != EINTR)
        {
            return false;
        }
    }
    return remaining == 0;
}

static int handle_line(int fd, char *line, bool robot)
{
    if (strncmp(line, "STOP OK ", 8) == 0)
    {
        dprintf(STDOUT_FILENO, "%s\n", line);
        return 0;
    }
    if (strncmp(line, "STOP ERROR ", 11) == 0 || strncmp(line, "ERROR ", 6) == 0)
    {
        dprintf(STDERR_FILENO, "%s\n", line);
        return 1;
    }
    if (robot && strncmp(line, "STEP ", 5) == 0)
    {
        int fields[4];
        char *rest;
        char *word = strtok_r(line + 5, " ", &rest);
        for (int i = 0; i < 4; ++i)
        {
            if (word == NULL || !wh_tcp_int(word, 0, INT_MAX, &fields[i]))
            {
                dprintf(STDERR_FILENO, "Invalid STEP message\n");
                return 1;
            }
            word = strtok_r(NULL, " ", &rest);
        }
        if (word != NULL || fields[1] > 1)
        {
            dprintf(STDERR_FILENO, "Invalid STEP message\n");
            return 1;
        }
        int tick = fields[0];
        /* an unassigned robot requests a step only when it can recharge */
        const char *verb = fields[1] || fields[2] < fields[3] ? "ACT" : "WAIT";
        char action[64];
        snprintf(action, sizeof(action), "%s %d\n", verb, tick);
        if (!send_line(fd, action))
        {
            return wh_signal_stop ? 0 : 1;
        }
        dprintf(STDOUT_FILENO, "REQUEST tick=%d action=%s\n", tick, verb);
    }
    else
    {
        dprintf(STDOUT_FILENO, "%s\n", line);
    }
    return -1;
}

static int run_client(int fd, bool robot)
{
    char line[WH_TCP_LINE];
    size_t used = 0;
    while (!wh_signal_stop)
    {
        struct pollfd socket = {.fd = fd, .events = POLLIN};
        int ready = poll(&socket, 1, 100);
        if (ready == 0 || (ready < 0 && errno == EINTR))
        {
            continue;
        }
        if (ready < 0)
        {
            perror("poll");
            return 1;
        }
        char buffer[4096];
        ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            if (wh_signal_stop)
            {
                break;
            }
            dprintf(STDERR_FILENO, "Disconnected before server sent STOP\n");
            return 1;
        }
        for (ssize_t i = 0; i < count; ++i)
        {
            if (buffer[i] == '\n')
            {
                line[used] = '\0';
                int status = handle_line(fd, line, robot);
                if (status >= 0)
                {
                    return status;
                }
                used = 0;
            }
            else if (buffer[i] == '\0' || used == sizeof(line) - 1)
            {
                dprintf(STDERR_FILENO, "Invalid protocol line\n");
                return 1;
            }
            else
            {
                line[used++] = buffer[i];
            }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
#ifdef WH_TCP_ROBOT
    const bool robot = true;
#else
    const bool robot = false;
#endif
    Options options = {.ip = "127.0.0.1", .port = 9000};
    int parsed = read_options(&options, argc, argv, robot);
    if (parsed != 0)
    {
        return parsed > 0 ? 0 : 2;
    }
    struct sockaddr_in address;
    if (!wh_tcp_address(&address, options.ip, options.port))
    {
        dprintf(STDERR_FILENO, "Invalid IPv4 address: %s\n", options.ip);
        return 2;
    }
    if (wh_tcp_signals() < 0)
    {
        perror("sigaction");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        if (!wh_signal_stop)
        {
            perror("connect");
        }
        close(fd);
        return wh_signal_stop ? 0 : 1;
    }

    char hello[64];
    if (robot)
    {
        snprintf(hello, sizeof(hello), "HELLO ROBOT %d\n", options.id);
    }
    else
    {
        strcpy(hello, "HELLO OBSERVER\n");
    }
    int status = send_line(fd, hello) ? run_client(fd, robot) : 1;
    close(fd);
    return wh_signal_stop ? 0 : status;
}
