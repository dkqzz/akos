#include "warehouse.h"
#include "config.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

static volatile sig_atomic_t interrupted = 0;

static void on_sigint(int signal_number)
{
    /* the handler sets a flag; cleanup stays in main */
    if (signal_number == SIGINT)
    {
        interrupted = 1;
        wh_signal_stop = 1;
    }
}

static void print_usage(const char *program)
{
    const char *usage =
        "Usage: %s [options]\n"
        "  --config PATH    configuration file (default: data/demo.cfg)\n"
        "  --log PATH       log file (default: warehouse.log)\n"
        "  --seed N         reproducible seed override\n"
        "  --delay MS       delay between ticks\n"
        "  --max-ticks N    simulation limit\n"
        "  --quiet          suppress terminal events and map\n"
        "  --help           show this help\n";
    dprintf(STDOUT_FILENO, usage, program);
}

static bool parse_integer(const char *text, int *value)
{
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < 0 || parsed > 1000000L)
    {
        return false;
    }
    *value = (int)parsed;
    return true;
}

int main(int argc, char **argv)
{
    const char *config_path = "data/demo.cfg";
    const char *log_path = "warehouse.log";
    int seed_override = -1;
    int delay_override = -1;
    int max_ticks_override = -1;
    bool quiet = false;

    for (int index = 1; index < argc; ++index)
    {
        const char *argument = argv[index];
        if (strcmp(argument, "--help") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argument, "--quiet") == 0)
        {
            quiet = true;
            continue;
        }
        if (index + 1 >= argc)
        {
            dprintf(STDERR_FILENO, "Missing value for %s\n", argument);
            return 2;
        }
        const char *value = argv[++index];
        if (strcmp(argument, "--config") == 0)
        {
            config_path = value;
        }
        else if (strcmp(argument, "--log") == 0)
        {
            log_path = value;
        }
        else if (strcmp(argument, "--seed") == 0)
        {
            if (!parse_integer(value, &seed_override))
            {
                dprintf(STDERR_FILENO, "Invalid seed: %s\n", value);
                return 2;
            }
        }
        else if (strcmp(argument, "--delay") == 0)
        {
            if (!parse_integer(value, &delay_override))
            {
                dprintf(STDERR_FILENO, "Invalid delay: %s\n", value);
                return 2;
            }
        }
        else if (strcmp(argument, "--max-ticks") == 0)
        {
            if (!parse_integer(value, &max_ticks_override) || max_ticks_override == 0)
            {
                dprintf(STDERR_FILENO, "Invalid max-ticks: %s\n", value);
                return 2;
            }
        }
        else
        {
            dprintf(STDERR_FILENO, "Unknown option: %s\n", argument);
            return 2;
        }
    }

    Warehouse warehouse;
    wh_init(&warehouse, 1, 1);
    if (wh_load_config(&warehouse, config_path) != 0)
    {
        dprintf(STDERR_FILENO, "Could not load config: %s\n", config_path);
        return 2;
    }
    if (seed_override >= 0)
    {
        warehouse.seed = seed_override;
    }
    if (delay_override >= 0)
    {
        warehouse.delay_ms = delay_override;
    }
    if (max_ticks_override > 0)
    {
        warehouse.max_ticks = max_ticks_override;
    }
    warehouse.quiet = quiet;
    srand((unsigned int)warehouse.seed);

    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0)
    {
        dprintf(STDERR_FILENO, "Could not open log: %s\n", log_path);
        return 2;
    }
    warehouse.output_fd = STDOUT_FILENO;
    warehouse.log_fd = log_fd;
    signal(SIGINT, on_sigint);

    if (!warehouse.quiet)
    {
        dprintf(STDOUT_FILENO,
                "Warehouse simulation: %dx%d, robots=%d, tasks=%d, seed=%d\n",
                warehouse.width, warehouse.height, warehouse.robot_count,
                warehouse.task_count, warehouse.seed);
        wh_print_map(&warehouse);
    }
    int run_result = wh_run(&warehouse, warehouse.max_ticks, warehouse.delay_ms);
    if (interrupted != 0)
    {
        wh_request_stop(&warehouse);
    }
    close(log_fd);
    return run_result;
}
