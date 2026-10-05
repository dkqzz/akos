#include "config.h"

#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct
{
    int id;
    int row;
    int col;
    int capacity;
    int energy;
    int max_energy;
} ConfigRobot;

typedef struct
{
    int id;
    int pickup_row;
    int pickup_col;
    int dropoff_row;
    int dropoff_col;
    int weight;
} ConfigTask;

static char *trim(char *text)
{
    while (*text != '\0' && isspace((unsigned char)*text) != 0)
    {
        ++text;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]) != 0)
    {
        --end;
        *end = '\0';
    }
    return text;
}

static int read_file(const char *path, char *buffer, size_t capacity)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        return -1;
    }
    /* read through a descriptor */
    size_t used = 0;
    while (used + 1 < capacity)
    {
        ssize_t count = read(fd, buffer + used, capacity - used - 1);
        if (count == 0)
        {
            break;
        }
        if (count < 0)
        {
            close(fd);
            return -1;
        }
        used += (size_t)count;
    }
    close(fd);
    buffer[used] = '\0';
    return used >= capacity - 1 ? -1 : 0;
}

static int parse_int(const char *text, int *result)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *trim(end) != '\0' || value < -2147483647L ||
        value > 2147483647L)
    {
        return -1;
    }
    *result = (int)value;
    return 0;
}

static WhCell parse_cell(char symbol)
{
    switch (symbol)
    {
    case '.':
        return WH_FLOOR;
    case '#':
        return WH_OBSTACLE;
    case 'P':
        return WH_PICKUP;
    case 'D':
        return WH_DROPOFF;
    case 'C':
        return WH_CHARGE;
    case 'N':
        return WH_NARROW;
    default:
        return WH_OBSTACLE;
    }
}

int wh_load_config(Warehouse *warehouse, const char *path)
{
    char buffer[32768];
    if (read_file(path, buffer, sizeof(buffer)) != 0)
    {
        return -1;
    }

    int width = warehouse->width;
    int height = warehouse->height;
    int move_energy = warehouse->move_energy;
    int charge_rate = warehouse->charge_rate;
    int load_ticks = warehouse->load_ticks;
    int unload_ticks = warehouse->unload_ticks;
    int max_wait = warehouse->max_wait_ticks;
    int max_ticks = warehouse->max_ticks;
    int seed = warehouse->seed;
    char grid[WH_MAX_HEIGHT][WH_MAX_WIDTH + 1] = {{0}};
    int grid_rows = 0;
    ConfigRobot robots[WH_MAX_ROBOTS];
    int robot_count = 0;
    ConfigTask tasks[WH_MAX_TASKS];
    int task_count = 0;

    char *save_line = NULL;
    for (char *line = strtok_r(buffer, "\n", &save_line); line != NULL;
         line = strtok_r(NULL, "\n", &save_line))
    {
        char *clean = trim(line);
        if (*clean == '\0' || *clean == ';' || *clean == '#')
        {
            continue;
        }
        char *separator = strchr(clean, '=');
        if (separator == NULL)
        {
            return -1;
        }
        *separator = '\0';
        char *key = trim(clean);
        char *value = trim(separator + 1);
        int parsed = 0;
        if (strcmp(key, "width") == 0)
        {
            if (parse_int(value, &width) != 0)
            {
                return -1;
            }
        }
        else if (strcmp(key, "height") == 0)
        {
            if (parse_int(value, &height) != 0)
            {
                return -1;
            }
        }
        else if (strcmp(key, "grid") == 0)
        {
            if (grid_rows >= WH_MAX_HEIGHT || strlen(value) > WH_MAX_WIDTH)
            {
                return -1;
            }
            strncpy(grid[grid_rows], value, WH_MAX_WIDTH);
            grid[grid_rows][WH_MAX_WIDTH] = '\0';
            grid_rows++;
        }
        else if (strcmp(key, "move_energy") == 0)
        {
            parsed = parse_int(value, &move_energy);
        }
        else if (strcmp(key, "charge_rate") == 0)
        {
            parsed = parse_int(value, &charge_rate);
        }
        else if (strcmp(key, "load_ticks") == 0)
        {
            parsed = parse_int(value, &load_ticks);
        }
        else if (strcmp(key, "unload_ticks") == 0)
        {
            parsed = parse_int(value, &unload_ticks);
        }
        else if (strcmp(key, "max_wait") == 0)
        {
            parsed = parse_int(value, &max_wait);
        }
        else if (strcmp(key, "max_ticks") == 0)
        {
            parsed = parse_int(value, &max_ticks);
        }
        else if (strcmp(key, "seed") == 0)
        {
            parsed = parse_int(value, &seed);
        }
        else if (strcmp(key, "robot") == 0)
        {
            if (robot_count >= WH_MAX_ROBOTS ||
                sscanf(value, "%d,%d,%d,%d,%d,%d", &robots[robot_count].id,
                       &robots[robot_count].row, &robots[robot_count].col,
                       &robots[robot_count].capacity, &robots[robot_count].energy,
                       &robots[robot_count].max_energy) != 6)
            {
                return -1;
            }
            robot_count++;
        }
        else if (strcmp(key, "task") == 0)
        {
            if (task_count >= WH_MAX_TASKS ||
                sscanf(value, "%d,%d,%d,%d,%d,%d", &tasks[task_count].id,
                       &tasks[task_count].pickup_row, &tasks[task_count].pickup_col,
                       &tasks[task_count].dropoff_row, &tasks[task_count].dropoff_col,
                       &tasks[task_count].weight) != 6)
            {
                return -1;
            }
            task_count++;
        }
        else
        {
            return -1;
        }
        if (parsed != 0)
        {
            return -1;
        }
    }

    if (width < 1 || width > WH_MAX_WIDTH || height < 1 || height > WH_MAX_HEIGHT ||
        grid_rows != height || move_energy < 0 || charge_rate < 0 || load_ticks < 0 ||
        unload_ticks < 0 || max_wait < 0 || max_ticks < 1)
    {
        return -1;
    }

    int output_fd = warehouse->output_fd;
    int log_fd = warehouse->log_fd;
    bool quiet = warehouse->quiet;
    wh_init(warehouse, width, height);
    warehouse->output_fd = output_fd;
    warehouse->log_fd = log_fd;
    warehouse->quiet = quiet;
    warehouse->move_energy = move_energy;
    warehouse->charge_rate = charge_rate;
    warehouse->load_ticks = load_ticks;
    warehouse->unload_ticks = unload_ticks;
    warehouse->max_wait_ticks = max_wait;
    warehouse->max_ticks = max_ticks;
    warehouse->seed = seed;

    for (int row = 0; row < height; ++row)
    {
        if ((int)strlen(grid[row]) != width)
        {
            return -1;
        }
        for (int col = 0; col < width; ++col)
        {
            char symbol = grid[row][col];
            if (symbol != '.' && symbol != '#' && symbol != 'P' && symbol != 'D' &&
                symbol != 'C' && symbol != 'N')
            {
                return -1;
            }
            wh_set_cell(warehouse, row, col, parse_cell(symbol));
        }
    }
    for (int index = 0; index < robot_count; ++index)
    {
        if (wh_add_robot(warehouse, robots[index].id, robots[index].row,
                         robots[index].col, robots[index].capacity, robots[index].energy,
                         robots[index].max_energy) < 0)
        {
            return -1;
        }
    }
    for (int index = 0; index < task_count; ++index)
    {
        if (wh_add_task(warehouse, tasks[index].id, tasks[index].pickup_row,
                        tasks[index].pickup_col, tasks[index].dropoff_row,
                        tasks[index].dropoff_col, tasks[index].weight) < 0)
        {
            return -1;
        }
    }
    return 0;
}
