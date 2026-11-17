#define _POSIX_C_SOURCE 200809L

#include "tcp.h"
#include "warehouse.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>

bool wh_tcp_int(const char *text, int min, int max, int *value)
{
    char *end;
    errno = 0;
    long number = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || number < min || number > max)
    {
        return false;
    }
    *value = (int)number;
    return true;
}

bool wh_tcp_address(struct sockaddr_in *address, const char *ip, int port)
{
    *address = (struct sockaddr_in){.sin_family = AF_INET,
                                    .sin_port = htons((uint16_t)port)};
    return inet_pton(AF_INET, ip, &address->sin_addr) == 1;
}

int wh_tcp_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void request_stop(int signal_number)
{
    (void)signal_number;
    wh_signal_stop = 1;
}

int wh_tcp_signals(void)
{
    struct sigaction action = {.sa_handler = request_stop};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0)
    {
        return -1;
    }
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
}

int64_t wh_tcp_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
