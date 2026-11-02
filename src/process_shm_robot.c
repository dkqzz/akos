#define _POSIX_C_SOURCE 200809L

#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void stop(int signal_number)
{
    if (signal_number == SIGINT || signal_number == SIGTERM)
    {
        wh_signal_stop = 1;
    }
}

int main(int argc, char **argv)
{
    const char *run_id = NULL;
    const char *log = "warehouse-process-shm.log";
    int robot_id = -1;
    for (int i = 1; i < argc; ++i)
    {
        if (i + 1 >= argc)
        {
            return 2;
        }
        const char *value = argv[++i];
        if (strcmp(argv[i - 1], "--run-id") == 0)
            run_id = value;
        else if (strcmp(argv[i - 1], "--robot-id") == 0)
            robot_id = atoi(value);
        else if (strcmp(argv[i - 1], "--log") == 0)
            log = value;
        else
            return 2;
    }
    if (run_id == NULL || robot_id < 0)
    {
        dprintf(STDERR_FILENO,
                "usage: %s --run-id ID --robot-id N [--log PATH]\n", argv[0]);
        return 2;
    }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    WhProcessShared *shared = NULL;
    int shm_fd = -1;
    WhProcessSem sems[5] = {{0}};
    WhProcessSem exit_sem = {0};
    int status = 1;
    int log_fd = open(log, O_WRONLY | O_APPEND);
    if (log_fd < 0 || wh_process_shm_open(run_id, &shared, &shm_fd) != 0)
    {
        perror("connect to controller");
        if (log_fd >= 0)
            close(log_fd);
        return 1;
    }
    int index = -1;
    for (int i = 0; i < shared->warehouse.robot_count; ++i)
    {
        if (shared->warehouse.robots[i].id == robot_id)
            index = i;
    }
    const char *kind[] = {"state", "output", "ready", "wake", "done"};
    for (int i = 0; i < 5; ++i)
    {
        char name[WH_PROCESS_NAME_SIZE];
        int sem_robot_id = i < 2 ? -1 : robot_id;
        if (wh_process_name(name, sizeof(name), run_id, kind[i], sem_robot_id) != 0 ||
            wh_process_sem_open(&sems[i], name) != 0)
        {
            perror("open semaphore");
            goto cleanup;
        }
    }
    char exit_name[WH_PROCESS_NAME_SIZE];
    if (index < 0 || wh_process_name(exit_name, sizeof(exit_name), run_id, "exit", robot_id) != 0 ||
        wh_process_sem_open(&exit_sem, exit_name) != 0)
    {
        dprintf(STDERR_FILENO, "robot id not found: %d\n", robot_id);
        goto cleanup;
    }
    if (wh_process_sem_lock(sems[0].handle) != 0)
        goto cleanup;
    if (shared->ready[index])
    {
        wh_process_sem_unlock(sems[0].handle);
        dprintf(STDERR_FILENO, "robot is already connected: %d\n", robot_id);
        goto cleanup;
    }
    shared->ready[index] = 1;
    wh_process_sem_unlock(sems[0].handle);
    sem_post(sems[2].handle);
    wh_process_worker(shared, index, sems[3].handle, sems[4].handle,
                      sems[0].handle, sems[1].handle, log_fd);
    sem_post(exit_sem.handle);
    status = 0;

cleanup:
    wh_process_sem_close(&exit_sem);
    for (int i = 0; i < 5; ++i)
        wh_process_sem_close(&sems[i]);
    if (shared != NULL)
        wh_process_shm_close(shm_fd, shared);
    close(log_fd);
    return status;
}
