#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "io.h"
#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define SEM_START 0
#define SEM_WAKE 1
#define SEM_DONE 2
#define SEM_EXIT 3
static void stop(int signal_number)
{
    if (signal_number == SIGINT || signal_number == SIGTERM)
    {
        wh_signal_stop = 1;
    }
}

static void usage(const char *program)
{
    dprintf(STDOUT_FILENO,
            "usage: %s --run-id ID [--config PATH] [--log PATH] [--delay MS] [--max-ticks N] [--quiet]\n",
            program);
}

int main(int argc, char **argv)
{
    const char *config = "data/demo.cfg";
    const char *log = "warehouse-process-shm.log";
    const char *run_id = NULL;
    int delay = -1;
    int max_ticks = -1;
    bool quiet = false;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--quiet") == 0)
        {
            quiet = true;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0)
        {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc)
        {
            usage(argv[0]);
            return 2;
        }
        const char *value = argv[++i];
        if (strcmp(argv[i - 1], "--config") == 0)
            config = value;
        else if (strcmp(argv[i - 1], "--log") == 0)
            log = value;
        else if (strcmp(argv[i - 1], "--run-id") == 0)
            run_id = value;
        else if (strcmp(argv[i - 1], "--delay") == 0)
            delay = atoi(value);
        else if (strcmp(argv[i - 1], "--max-ticks") == 0)
            max_ticks = atoi(value);
        else
            return 2;
    }
    if (run_id == NULL)
    {
        usage(argv[0]);
        return 2;
    }

    Warehouse source;
    wh_init(&source, 1, 1);
    if (wh_load_config(&source, config) != 0)
    {
        perror("config");
        return 2;
    }
    if (delay >= 0)
        source.delay_ms = delay;
    if (max_ticks > 0)
        source.max_ticks = max_ticks;
    source.quiet = quiet;
    source.output_fd = quiet ? -1 : STDOUT_FILENO;
    int log_fd = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (log_fd < 0)
    {
        perror("log");
        return 2;
    }
    source.log_fd = log_fd;
    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    WhProcessShared *shared = NULL;
    int shm_fd = -1;
    WhProcessSem locks[2] = {{0}};
    WhProcessSem sems[WH_MAX_ROBOTS][4] = {{{0}}};
    bool connected[WH_MAX_ROBOTS] = {false};
    int status = 1;
    char name[WH_PROCESS_NAME_SIZE];
    if (wh_process_name(name, sizeof(name), run_id, "state", -1) != 0 ||
        wh_process_sem_create(&locks[0], name, 1U, true, NULL) != 0 ||
        wh_process_name(name, sizeof(name), run_id, "output", -1) != 0 ||
        wh_process_sem_create(&locks[1], name, 1U, true, NULL) != 0)
    {
        perror("semaphore");
        goto cleanup;
    }
    for (int i = 0; i < source.robot_count; ++i)
    {
        const char *kind[] = {"ready", "wake", "done", "exit"};
        for (int k = 0; k < 4; ++k)
        {
            if (wh_process_name(name, sizeof(name), run_id, kind[k],
                                source.robots[i].id) != 0 ||
                wh_process_sem_create(&sems[i][k], name, 0U, true, NULL) != 0)
            {
                perror("semaphore");
                goto cleanup;
            }
        }
    }
    if (wh_process_shm_create(run_id, &shared, &shm_fd) != 0)
    {
        perror("shared memory");
        goto cleanup;
    }
    shared->warehouse = source;
    shared->warehouse.sync_ready = false;

    wh_output_bind(&shared->warehouse, source.output_fd, log_fd, quiet,
                   locks[1].handle);
    if (!quiet)
    {
        wh_print_map(&shared->warehouse);
    }
    status = 0;
    for (int i = 0; i < source.robot_count; ++i)
    {
        int result = wh_process_wait(sems[i][SEM_START].handle, 5000U, true);
        if (result != 0)
        {
            dprintf(STDERR_FILENO,
                    "not all robots connected; run ./scripts/run_process_shm.sh or check --run-id\n");
            status = 1;
            break;
        }
        connected[i] = true;
    }
    if (status == 0)
    {
        sem_t *wake[WH_MAX_ROBOTS];
        sem_t *done[WH_MAX_ROBOTS];
        for (int i = 0; i < source.robot_count; ++i)
        {
            wake[i] = sems[i][SEM_WAKE].handle;
            done[i] = sems[i][SEM_DONE].handle;
        }
        status = wh_process_run(&shared->warehouse, source.max_ticks,
                                source.delay_ms, wake, done,
                                source.robot_count, locks[0].handle);
    }

cleanup:
    if (shared != NULL)
    {
        if (wh_process_sem_lock(locks[0].handle) == 0)
        {
            shared->warehouse.stop_requested = true;
            for (int i = 0; i < source.robot_count; ++i)
            {
                connected[i] = connected[i] || shared->ready[i] != 0;
            }
            wh_process_sem_unlock(locks[0].handle);
        }
        for (int i = 0; i < source.robot_count; ++i)
        {
            if (connected[i] && sems[i][SEM_WAKE].handle != NULL)
                sem_post(sems[i][SEM_WAKE].handle);
        }
        for (int i = 0; i < source.robot_count; ++i)
        {
            if (connected[i] &&
                wh_process_wait(sems[i][SEM_EXIT].handle, 3000U, false) != 0)
            {
                status = 1;
            }
        }
        wh_output_unbind();
        wh_process_shm_close(shm_fd, shared);
        wh_process_shm_unlink(run_id);
    }
    for (int i = 0; i < source.robot_count; ++i)
    {
        for (int k = 0; k < 4; ++k)
            wh_process_sem_close(&sems[i][k]);
    }
    wh_process_sem_close(&locks[0]);
    wh_process_sem_close(&locks[1]);
    close(log_fd);
    return status;
}
