#include "warehouse.h"

#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

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

    if (warehouse->sync_ready)
    {
        pthread_mutex_lock(&warehouse->output_mutex);
    }
    if (!warehouse->quiet && warehouse->output_fd >= 0)
    {
        write_all(warehouse->output_fd, text, (size_t)length);
    }
    if (warehouse->log_fd >= 0 && warehouse->log_fd != warehouse->output_fd)
    {
        write_all(warehouse->log_fd, text, (size_t)length);
    }
    if (warehouse->sync_ready)
    {
        pthread_mutex_unlock(&warehouse->output_mutex);
    }
}

void wh_print_map(const Warehouse *warehouse)
{
    if (warehouse->quiet || warehouse->output_fd < 0)
    {
        return;
    }
    Warehouse *mutable_warehouse = (Warehouse *)warehouse;
    if (mutable_warehouse->sync_ready)
    {
        pthread_mutex_lock(&mutable_warehouse->output_mutex);
    }
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
        write_all(warehouse->output_fd, line, (size_t)length);
    }
    if (mutable_warehouse->sync_ready)
    {
        pthread_mutex_unlock(&mutable_warehouse->output_mutex);
    }
}
