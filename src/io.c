#include "warehouse.h"
#include "io.h"

#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

typedef struct
{
    Warehouse *warehouse;
    int output_fd;
    int log_fd;
    bool quiet;
    sem_t *lock;
} WhOutput;

static _Thread_local WhOutput output;

void wh_output_bind(Warehouse *warehouse, int output_fd, int log_fd,
                    bool quiet, sem_t *lock)
{
    output = (WhOutput){warehouse, output_fd, log_fd, quiet, lock};
}

void wh_output_unbind(void)
{
    output = (WhOutput){0};
}

static int output_fd(const Warehouse *warehouse)
{
    return output.warehouse == warehouse ? output.output_fd : warehouse->output_fd;
}

static int log_fd(const Warehouse *warehouse)
{
    return output.warehouse == warehouse ? output.log_fd : warehouse->log_fd;
}

static bool quiet_output(const Warehouse *warehouse)
{
    return output.warehouse == warehouse ? output.quiet : warehouse->quiet;
}

static void output_lock(Warehouse *warehouse)
{
    if (output.warehouse == warehouse && output.lock != NULL)
    {
        while (sem_wait(output.lock) != 0 && errno == EINTR)
        {
        }
    }
    else if (warehouse->sync_ready)
    {
        pthread_mutex_lock(&warehouse->output_mutex);
    }
}

static void output_unlock(Warehouse *warehouse)
{
    if (output.warehouse == warehouse && output.lock != NULL)
    {
        sem_post(output.lock);
    }
    else if (warehouse->sync_ready)
    {
        pthread_mutex_unlock(&warehouse->output_mutex);
    }
}

/* one place for terminal and log output */

static void write_all(int fd, const char *text, size_t length)
{
    size_t offset = 0;
    while (offset < length)
    {
        ssize_t written = write(fd, text + offset, length - offset);
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return;
        }
        if (written == 0)
        {
            return;
        }
        offset += (size_t)written;
    }
}

void emit_event(Warehouse *warehouse, const char *format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (length <= 0)
    {
        return;
    }
    if (length >= (int)sizeof(text))
    {
        length = (int)sizeof(text) - 1;
    }

    output_lock(warehouse);
    int terminal = output_fd(warehouse);
    int log = log_fd(warehouse);
    if (!quiet_output(warehouse) && terminal >= 0)
    {
        write_all(terminal, text, (size_t)length);
    }
    if (log >= 0 && log != terminal)
    {
        write_all(log, text, (size_t)length);
    }
    output_unlock(warehouse);
}

void wh_print_map(const Warehouse *warehouse)
{
    int terminal = output_fd(warehouse);
    if (quiet_output(warehouse) || terminal < 0)
    {
        return;
    }
    Warehouse *mutable_warehouse = (Warehouse *)warehouse;
    output_lock(mutable_warehouse);
    char line[WH_MAX_WIDTH + 2];
    for (int row = 0; row < warehouse->height; row++)
    {
        int length = 0;
        for (int col = 0; col < warehouse->width; col++)
        {
            char symbol = (char)warehouse->cells[row][col];
            for (int robot = 0; robot < warehouse->robot_count; robot++)
            {
                const WhRobot *item = &warehouse->robots[robot];
                if (item->position.row == row && item->position.col == col)
                {
                    symbol = item->id < 10 ? (char)('0' + item->id) : 'R';
                }
            }
            line[length] = symbol;
            length++;
        }
        line[length] = '\n';
        length++;
        write_all(terminal, line, (size_t)length);
    }
    output_unlock(mutable_warehouse);
}
