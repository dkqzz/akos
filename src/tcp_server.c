#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "io.h"
#include "tcp.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum
{
    MAX_PEERS = WH_MAX_ROBOTS + 64,
    OUTPUT_CAPACITY = 32768,
    HANDSHAKE_MS = 5000,
    SHUTDOWN_MS = 1000
};

typedef enum
{
    REGISTERING,
    ROBOT,
    OBSERVER,
    REJECTED
} Role;

typedef struct
{
    int fd;
    Role role;
    int robot_index;
    int64_t handshake_until;
    char input[WH_TCP_LINE];
    size_t input_used;
    char output[OUTPUT_CAPACITY];
    size_t output_start;
    size_t output_end;
    bool overflow;
} Peer;

typedef struct
{
    const char *ip;
    const char *config;
    const char *log;
    int port;
    int delay_ms;
    int max_ticks;
    int connect_ms;
    int response_ms;
    bool observers;
} Options;

typedef struct
{
    Warehouse warehouse;
    Peer peers[MAX_PEERS];
    bool online[WH_MAX_ROBOTS];
    bool waiting[WH_MAX_ROBOTS];
    bool acted[WH_MAX_ROBOTS];
    bool dispatching;
    bool observers;
    bool tick_open;
    bool started;
    int online_count;
    int response_ms;
    int connect_ms;
    int log;
    int64_t idle_until;
    int64_t tick_at;
    int64_t response_until;
} Server;

static int read_options(Options *options, int argc, char **argv)
{
    for (int index = 1; index < argc; ++index)
    {
        const char *name = argv[index];
        if (strcmp(name, "--help") == 0)
        {
            dprintf(STDOUT_FILENO,
                    "Usage: %s [--ip IPV4] [--port PORT] [--config PATH] [--log PATH]\n"
                    "       [--delay MS] [--max-ticks N] [--connect-timeout MS]\n"
                    "       [--response-timeout MS] [--no-observers]\n",
                    argv[0]);
            return 1;
        }
        if (strcmp(name, "--no-observers") == 0)
        {
            options->observers = false;
            continue;
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
        else if (strcmp(name, "--config") == 0)
        {
            options->config = value;
        }
        else if (strcmp(name, "--log") == 0)
        {
            options->log = value;
        }
        else if ((strcmp(name, "--port") == 0 &&
                  wh_tcp_int(value, 0, 65535, &options->port)) ||
                 (strcmp(name, "--delay") == 0 &&
                  wh_tcp_int(value, 0, 1000000, &options->delay_ms)) ||
                 (strcmp(name, "--max-ticks") == 0 &&
                  wh_tcp_int(value, 1, 1000000, &options->max_ticks)) ||
                 (strcmp(name, "--connect-timeout") == 0 &&
                  wh_tcp_int(value, 1, 1000000, &options->connect_ms)) ||
                 (strcmp(name, "--response-timeout") == 0 &&
                  wh_tcp_int(value, 1, 1000000, &options->response_ms)))
        {
            continue;
        }
        else
        {
            dprintf(STDERR_FILENO, "Invalid option: %s %s\n", name, value);
            return -1;
        }
    }
    return 0;
}

/* each socket has an independent queue so monitor output cannot hold up a tick */
static void enqueue(Peer *peer, const char *text)
{
    size_t size = strlen(text);
    size_t pending = peer->output_end - peer->output_start;
    if (size > sizeof(peer->output) - pending)
    {
        peer->overflow = true;
        return;
    }
    if (size > sizeof(peer->output) - peer->output_end)
    {
        memmove(peer->output, peer->output + peer->output_start, pending);
        peer->output_start = 0;
        peer->output_end = pending;
    }
    memcpy(peer->output + peer->output_end, text, size);
    peer->output_end += size;
}

static void publish_event(void *context, const char *text)
{
    Server *server = context;
    if (server->dispatching)
    {
        dprintf(STDOUT_FILENO, "%s", text);
        if (!server->observers)
        {
            dprintf(server->log, "%s", text);
        }
    }
    for (int i = 0; i < MAX_PEERS; ++i)
    {
        Peer *peer = &server->peers[i];
        if (peer->fd >= 0 && peer->role == OBSERVER)
        {
            enqueue(peer, text);
        }
    }
}

static const char *robot_state(WhRobotState state)
{
    const char *names[] = {"idle", "to_pickup", "loading", "to_dropoff", "unloading", "charging"};
    return names[state];
}

static void send_robot_state(Server *server, Peer *peer, int index)
{
    const Warehouse *warehouse = &server->warehouse;
    const WhRobot *robot = &warehouse->robots[index];
    char line[WH_TCP_LINE];
    snprintf(line, sizeof(line),
             "ROBOT tick=%d id=%d position=(%d,%d) energy=%d/%d state=%s task=%d carrying=%d online=%d\n",
             warehouse->tick, robot->id, robot->position.row, robot->position.col,
             robot->energy, robot->max_energy, robot_state(robot->state),
             robot->task_id, robot->carrying, server->online[index]);
    enqueue(peer, line);
}

static void send_snapshot(Server *server, Peer *peer)
{
    const Warehouse *warehouse = &server->warehouse;
    char line[WH_TCP_LINE];
    snprintf(line, sizeof(line),
             "SNAPSHOT tick=%d assigned=%d completed=%d unreachable=%d\n",
             warehouse->tick, warehouse->stats_assigned, warehouse->stats_completed,
             warehouse->stats_unreachable);
    enqueue(peer, line);
    snprintf(line, sizeof(line), "MAP tick=%d\n", warehouse->tick);
    enqueue(peer, line);
    for (int row = 0; row < warehouse->height; ++row)
    {
        wh_format_map_row(warehouse, row, line);
        line[warehouse->width] = '\n';
        line[warehouse->width + 1] = '\0';
        enqueue(peer, line);
    }
    for (int i = 0; i < warehouse->robot_count; ++i)
    {
        send_robot_state(server, peer, i);
    }
    const char *states[] = {"pending", "to_pickup", "loading", "to_dropoff",
                            "unloading", "completed", "unreachable"};
    for (int i = 0; i < warehouse->task_count; ++i)
    {
        const WhTask *task = &warehouse->tasks[i];
        snprintf(line, sizeof(line), "TASK id=%d state=%s assigned_robot=%d\n",
                 task->id, states[task->status], task->assigned_robot);
        enqueue(peer, line);
    }
    enqueue(peer, "END SNAPSHOT\n");
}

static void disconnect(Server *server, Peer *peer, const char *reason)
{
    close(peer->fd);
    peer->fd = -1;
    if (peer->role == ROBOT)
    {
        int index = peer->robot_index;
        server->online[index] = false;
        server->waiting[index] = false;
        server->acted[index] = false;
        if (--server->online_count == 0 && server->started)
        {
            server->idle_until = wh_tcp_now_ms() + server->connect_ms;
        }
        dprintf(STDOUT_FILENO, "SERVER robot=%d disconnected (%s)\n",
                server->warehouse.robots[index].id, reason);
        emit_event(&server->warehouse, "OFFLINE robot=%d reason=%s\n",
                   server->warehouse.robots[index].id, reason);
    }
}

static bool reject(Peer *peer, const char *reason)
{
    char line[WH_TCP_LINE];
    snprintf(line, sizeof(line), "ERROR %s\n", reason);
    enqueue(peer, line);
    peer->role = REJECTED;
    return true;
}

static bool command(Server *server, Peer *peer, const char *line)
{
    Warehouse *warehouse = &server->warehouse;
    if (peer->role == REGISTERING)
    {
        if (strcmp(line, "HELLO OBSERVER") == 0)
        {
            if (!server->observers)
            {
                return reject(peer, "observers disabled");
            }
            peer->role = OBSERVER;
            enqueue(peer, "WELCOME OBSERVER\n");
            send_snapshot(server, peer);
            return true;
        }
        const char prefix[] = "HELLO ROBOT ";
        int id;
        if (strncmp(line, prefix, sizeof(prefix) - 1) != 0 ||
            !wh_tcp_int(line + sizeof(prefix) - 1, 0, INT_MAX, &id))
        {
            return reject(peer, "invalid registration");
        }
        for (int i = 0; i < warehouse->robot_count; ++i)
        {
            if (warehouse->robots[i].id != id)
            {
                continue;
            }
            if (server->online[i])
            {
                return reject(peer, "robot already connected");
            }
            peer->role = ROBOT;
            peer->robot_index = i;
            server->online[i] = true;
            server->online_count++;
            enqueue(peer, "WELCOME ROBOT\n");
            send_robot_state(server, peer, i);
            dprintf(STDOUT_FILENO, "SERVER robot=%d connected\n", id);
            emit_event(warehouse, "ONLINE robot=%d\n", id);
            return true;
        }
        return reject(peer, "unknown robot id");
    }

    bool advancing = strncmp(line, "ACT ", 4) == 0;
    if (peer->role == ROBOT && (advancing || strncmp(line, "WAIT ", 5) == 0))
    {
        int tick;
        if (!wh_tcp_int(line + (advancing ? 4 : 5), 0, INT_MAX, &tick))
        {
            return false;
        }
        int index = peer->robot_index;
        if (server->tick_open && server->waiting[index] && tick == warehouse->tick)
        {
            server->waiting[index] = false;
            server->acted[index] = advancing;
        }
        return true;
    }
    return false;
}

static bool read_peer(Server *server, Peer *peer)
{
    char chunk[WH_TCP_LINE];
    ssize_t received = recv(peer->fd, chunk, sizeof(chunk), 0);
    if (received <= 0)
    {
        return received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK);
    }
    for (ssize_t i = 0; i < received; ++i)
    {
        char byte = chunk[i];
        if (byte == '\n')
        {
            peer->input[peer->input_used] = '\0';
            peer->input_used = 0;
            if (!command(server, peer, peer->input))
            {
                return false;
            }
            if (peer->role == REJECTED)
            {
                break;
            }
        }
        else if (byte == '\0' || peer->input_used == sizeof(peer->input) - 1)
        {
            return false;
        }
        else
        {
            peer->input[peer->input_used++] = byte;
        }
    }
    return true;
}

static bool flush_peer(Peer *peer)
{
    ssize_t sent = send(peer->fd, peer->output + peer->output_start,
                        peer->output_end - peer->output_start, 0);
    if (sent > 0)
    {
        peer->output_start += (size_t)sent;
        if (peer->output_start == peer->output_end)
        {
            peer->output_start = 0;
            peer->output_end = 0;
        }
        return true;
    }
    return sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK);
}

static void accept_peer(Server *server, int listener)
{
    int fd = accept(listener, NULL, NULL);
    if (fd < 0)
    {
        return;
    }
    if (wh_tcp_nonblocking(fd) == 0)
    {
        for (int i = 0; i < MAX_PEERS; ++i)
        {
            Peer *peer = &server->peers[i];
            if (peer->fd < 0)
            {
                *peer = (Peer){.fd = fd, .role = REGISTERING, .handshake_until = wh_tcp_now_ms() + HANDSHAKE_MS};
                return;
            }
        }
    }
    close(fd);
}

/* reads and writes are bounded per socket and per poll iteration */
static int service(Server *server, int listener, int timeout_ms)
{
    struct pollfd fds[MAX_PEERS + 1] = {{.fd = listener, .events = POLLIN}};
    for (int i = 0; i < MAX_PEERS; ++i)
    {
        Peer *peer = &server->peers[i];
        fds[i + 1] = (struct pollfd){.fd = peer->fd};
        if (peer->role != REJECTED)
        {
            fds[i + 1].events = POLLIN;
        }
        if (peer->output_end > peer->output_start)
        {
            fds[i + 1].events |= POLLOUT;
        }
    }
    if (poll(fds, MAX_PEERS + 1, timeout_ms) < 0)
    {
        return errno == EINTR ? 0 : -1;
    }
    int64_t now = wh_tcp_now_ms();
    for (int i = 0; i < MAX_PEERS; ++i)
    {
        Peer *peer = &server->peers[i];
        if (peer->fd < 0)
        {
            continue;
        }
        short events = fds[i + 1].revents;
        if (listener < 0)
        {
            /* drain requests already in flight while delivering STOP */
            char discard[WH_TCP_LINE];
            if (peer->overflow || (events & (POLLERR | POLLHUP | POLLNVAL)) ||
                ((events & POLLIN) && recv(peer->fd, discard, sizeof(discard), 0) == 0) ||
                ((events & POLLOUT) && !flush_peer(peer)))
            {
                close(peer->fd);
                peer->fd = -1;
            }
            continue;
        }
        if (peer->overflow)
        {
            disconnect(server, peer, "output queue full");
        }
        else if (peer->role == REGISTERING && now >= peer->handshake_until)
        {
            disconnect(server, peer, "registration timeout");
        }
        else if ((events & (POLLERR | POLLHUP | POLLNVAL)) ||
                 ((events & POLLIN) && !read_peer(server, peer)) ||
                 ((events & POLLOUT) && !flush_peer(peer)))
        {
            disconnect(server, peer, "connection closed");
        }
        else if (peer->role == REJECTED && peer->output_end == peer->output_start)
        {
            disconnect(server, peer, "registration rejected");
        }
    }
    if (fds[0].revents & POLLIN)
    {
        accept_peer(server, listener);
    }
    return (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) ? -1 : 0;
}

static void assign_tasks(Server *server)
{
    server->dispatching = true;
    wh_assign_available_tasks(&server->warehouse, server->online);
    server->dispatching = false;
}

static void start_tick(Server *server, int64_t now)
{
    Warehouse *warehouse = &server->warehouse;
    assign_tasks(server);
    if (wh_all_done(warehouse))
    {
        return;
    }
    memset(server->acted, 0, sizeof(server->acted));
    memcpy(server->waiting, server->online, sizeof(server->waiting));
    server->tick_open = true;
    server->response_until = now + server->response_ms;
    for (int i = 0; i < MAX_PEERS; ++i)
    {
        Peer *peer = &server->peers[i];
        if (peer->fd >= 0 && peer->role == ROBOT)
        {
            int index = peer->robot_index;
            const WhRobot *robot = &warehouse->robots[index];
            bool on_charger = warehouse->cells[robot->position.row][robot->position.col] == WH_CHARGE;
            char step[128];
            snprintf(step, sizeof(step), "STEP %d %d %d %d\n", warehouse->tick,
                     robot->task_id != -1, robot->energy, on_charger ? robot->max_energy : 0);
            send_robot_state(server, peer, index);
            enqueue(peer, step);
        }
    }
}

static bool waiting_for_robot(const Server *server)
{
    for (int i = 0; i < server->warehouse.robot_count; ++i)
    {
        if (server->waiting[i])
        {
            return true;
        }
    }
    return false;
}

static int finish_tick(Server *server, int64_t now)
{
    if (waiting_for_robot(server) && now < server->response_until)
    {
        return 0;
    }
    for (int i = 0; i < MAX_PEERS; ++i)
    {
        Peer *peer = &server->peers[i];
        if (peer->fd >= 0 && peer->role == ROBOT && server->waiting[peer->robot_index])
        {
            disconnect(server, peer, "step timeout");
        }
    }
    server->tick_open = false;
    server->tick_at = now + server->warehouse.delay_ms;
    if (server->online_count == 0)
    {
        return 0;
    }
    if (wh_advance_tick(&server->warehouse, server->acted) < 0)
    {
        return -1;
    }
    for (int i = 0; i < MAX_PEERS; ++i)
    {
        Peer *peer = &server->peers[i];
        if (peer->fd >= 0 && peer->role == OBSERVER)
        {
            send_snapshot(server, peer);
        }
        else if (peer->fd >= 0 && peer->role == ROBOT)
        {
            send_robot_state(server, peer, peer->robot_index);
        }
    }
    return 0;
}

static int poll_interval(int64_t deadline, int64_t now)
{
    /* the cap bounds signal response even if the signal came just before poll */
    int64_t remaining = deadline - now;
    return remaining <= 0 ? 0 : remaining > 100 ? 100
                                                : (int)remaining;
}

static const char *run(Server *server, int listener, bool *failed)
{
    Warehouse *warehouse = &server->warehouse;
    char message[160];
    if (!wh_validate_invariants(warehouse, message, sizeof(message)))
    {
        emit_event(warehouse, "Initial invariant violation: %s\n", message);
        *failed = true;
        return "invariant violation";
    }
    server->idle_until = wh_tcp_now_ms() + server->connect_ms;
    while (!wh_signal_stop)
    {
        if (wh_all_done(warehouse))
        {
            return "all tasks resolved";
        }
        if (warehouse->tick >= warehouse->max_ticks)
        {
            return "tick limit";
        }
        int64_t now = wh_tcp_now_ms();
        if (!server->started && (server->online_count == warehouse->robot_count ||
                                 now >= server->idle_until))
        {
            server->started = true;
            /* resolve intrinsically impossible tasks even with no robot processes */
            if (server->online_count == 0)
            {
                assign_tasks(server);
            }
            if (wh_all_done(warehouse))
            {
                return "all tasks resolved";
            }
        }

        int64_t deadline = server->idle_until;
        if (server->tick_open)
        {
            if (finish_tick(server, now) < 0)
            {
                *failed = true;
                return "invariant violation";
            }
            deadline = server->tick_open ? server->response_until : now;
        }
        else if (server->started && server->online_count > 0)
        {
            if (now >= server->tick_at)
            {
                start_tick(server, now);
            }
            deadline = server->tick_open ? server->response_until : server->tick_at;
        }
        else if (server->started && now >= server->idle_until)
        {
            *failed = true;
            return "no robots connected";
        }
        if (service(server, listener, poll_interval(deadline, now)) < 0)
        {
            *failed = true;
            return "network error";
        }
    }
    return "interrupted";
}

static void shutdown_clients(Server *server, const char *reason, bool failed)
{
    char stop[WH_TCP_LINE];
    snprintf(stop, sizeof(stop), "STOP %s reason=%s\n", failed ? "ERROR" : "OK", reason);
    for (int i = 0; i < MAX_PEERS; ++i)
    {
        if (server->peers[i].fd >= 0)
        {
            enqueue(&server->peers[i], stop);
        }
    }

    int64_t until = wh_tcp_now_ms() + SHUTDOWN_MS;
    while (wh_tcp_now_ms() < until)
    {
        bool connected = false;
        for (int i = 0; i < MAX_PEERS; ++i)
        {
            connected |= server->peers[i].fd >= 0;
        }
        if (!connected || service(server, -1, poll_interval(until, wh_tcp_now_ms())) < 0)
        {
            break;
        }
    }
    for (int i = 0; i < MAX_PEERS; ++i)
    {
        if (server->peers[i].fd >= 0)
        {
            close(server->peers[i].fd);
        }
    }
}

static int listen_on(struct sockaddr_in *address)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
    {
        return -1;
    }
    int reuse = 1;
    socklen_t size = sizeof(*address);
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        wh_tcp_nonblocking(listener) < 0 ||
        bind(listener, (const struct sockaddr *)address, size) < 0 ||
        listen(listener, MAX_PEERS) < 0 ||
        getsockname(listener, (struct sockaddr *)address, &size) < 0)
    {
        int error = errno;
        close(listener);
        errno = error;
        return -1;
    }
    return listener;
}

static bool check_ids(const Warehouse *warehouse)
{
    for (int i = 0; i < warehouse->robot_count; ++i)
    {
        int id = warehouse->robots[i].id;
        if (id < 0)
        {
            dprintf(STDERR_FILENO, "Negative robot id: %d\n", id);
            return false;
        }
        for (int j = 0; j < i; ++j)
        {
            if (id == warehouse->robots[j].id)
            {
                dprintf(STDERR_FILENO, "Duplicate robot id: %d\n", id);
                return false;
            }
        }
    }
    for (int i = 0; i < warehouse->task_count; ++i)
    {
        int id = warehouse->tasks[i].id;
        if (id < 0)
        {
            dprintf(STDERR_FILENO, "Negative task id: %d\n", id);
            return false;
        }
        for (int j = 0; j < i; ++j)
        {
            if (id == warehouse->tasks[j].id)
            {
                dprintf(STDERR_FILENO, "Duplicate task id: %d\n", id);
                return false;
            }
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    Options options = {.ip = "127.0.0.1", .config = "data/demo.cfg", .log = "tcp.log", .port = 9000, .delay_ms = 100, .max_ticks = -1, .connect_ms = 10000, .response_ms = 2000, .observers = true};
    int parsed = read_options(&options, argc, argv);
    if (parsed != 0)
    {
        return parsed > 0 ? 0 : 2;
    }

    static Server server;
    Warehouse *warehouse = &server.warehouse;
    wh_init(warehouse, 1, 1);
    if (wh_load_config(warehouse, options.config) < 0)
    {
        dprintf(STDERR_FILENO, "Could not load config: %s\n", options.config);
        return 2;
    }
    if (!check_ids(warehouse))
    {
        return 2;
    }
    warehouse->delay_ms = options.delay_ms;
    if (options.max_ticks > 0)
    {
        warehouse->max_ticks = options.max_ticks;
    }
    server.observers = options.observers;
    server.connect_ms = options.connect_ms;
    server.response_ms = options.response_ms;
    for (int i = 0; i < MAX_PEERS; ++i)
    {
        server.peers[i].fd = -1;
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
    int listener = listen_on(&address);
    if (listener < 0)
    {
        perror("TCP listen");
        return 1;
    }
    int log = open(options.log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log < 0)
    {
        perror(options.log);
        close(listener);
        return 1;
    }
    server.log = log;
    wh_output_bind(warehouse, -1, server.observers ? log : -1, true, NULL);
    wh_output_set_event_sink(publish_event, &server);
    dprintf(STDOUT_FILENO, "LISTEN %s %u robots=%d\n", options.ip,
            (unsigned)ntohs(address.sin_port), warehouse->robot_count);

    bool failed = false;
    const char *reason = run(&server, listener, &failed);
    close(listener);

    for (int i = 0; i < MAX_PEERS; ++i)
    {
        if (server.peers[i].fd >= 0 && server.peers[i].role == OBSERVER)
        {
            send_snapshot(&server, &server.peers[i]);
        }
    }

    char summary[WH_TCP_LINE];
    snprintf(summary, sizeof(summary),
             "SUMMARY reason=%s ticks=%d assigned=%d completed=%d unreachable=%d moves=%d waits=%d\n",
             reason, warehouse->tick, warehouse->stats_assigned, warehouse->stats_completed,
             warehouse->stats_unreachable, warehouse->stats_moves, warehouse->stats_waits);
    emit_event(warehouse, "%s", summary);
    dprintf(STDOUT_FILENO, "%s", summary);
    if (!server.observers)
    {
        dprintf(log, "%s", summary);
    }
    shutdown_clients(&server, reason, failed);
    wh_output_unbind();
    close(log);
    return failed ? 1 : 0;
}
