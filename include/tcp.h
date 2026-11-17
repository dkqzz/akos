#ifndef WH_TCP_H
#define WH_TCP_H

#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>

#define WH_TCP_LINE 1024

bool wh_tcp_int(const char *text, int min, int max, int *value);
bool wh_tcp_address(struct sockaddr_in *address, const char *ip, int port);
int wh_tcp_nonblocking(int fd);
int wh_tcp_signals(void);
int64_t wh_tcp_now_ms(void);

#endif
