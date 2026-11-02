#define _POSIX_C_SOURCE 200809L

#include "process.h"
#include "io.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int stop_workers(WhProcessShared *shared, sem_t **wake,
                        const pid_t *workers, int count, sem_t *state_lock)
{
    int result = 0;
    if (wh_process_sem_lock(state_lock) == 0)
    {
        shared->warehouse.stop_requested = true;
        wh_process_sem_unlock(state_lock);
    }
    else
    {
        shared->warehouse.stop_requested = true;
    }
    for (int i = 0; i < count; ++i)
    {
        if (wake[i] != NULL)
        {
            sem_post(wake[i]);
        }
    }
    for (int i = 0; i < count; ++i)
    {
        if (workers[i] <= 0)
        {
            continue;
        }
        int status;
        while (waitpid(workers[i], &status, 0) < 0)
        {
            if (errno != EINTR)
            {
                result = -1;
                break;
            }
        }
    }
    return result;
}

int wh_run_process_parent(Warehouse *warehouse, int max_ticks, int delay_ms,
                          bool named_semaphores)
{
    char run_id[13];
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    (void)snprintf(run_id, sizeof(run_id), "p%05ld%05ld",
                   (long)(getpid() % 100000), now.tv_nsec % 100000L);

    WhProcessShared *shared = NULL;
    int shm_fd = -1;
    WhProcessSem locks[2] = {{0}};
    WhProcessSem sems[WH_MAX_ROBOTS * 2] = {{0}};
    sem_t *wake[WH_MAX_ROBOTS] = {0};
    sem_t *done[WH_MAX_ROBOTS] = {0};
    pid_t workers[WH_MAX_ROBOTS] = {0};
    int count = warehouse->robot_count;
    int status = 1;
    if (wh_process_shm_create(run_id, &shared, &shm_fd) != 0)
    {
        perror("process shared memory");
        return 1;
    }
    shared->warehouse = *warehouse;
    shared->warehouse.sync_ready = false;
    shared->warehouse.stop_requested = false;
    char name[WH_PROCESS_NAME_SIZE];
    if (wh_process_name(name, sizeof(name), run_id, "state", -1) != 0 ||
        wh_process_sem_create(&locks[0], name, 1U, true, NULL) != 0 ||
        wh_process_name(name, sizeof(name), run_id, "output", -1) != 0 ||
        wh_process_sem_create(&locks[1], name, 1U, true, NULL) != 0)
    {
        perror("process semaphore");
        goto cleanup;
    }
    for (int i = 0; i < count; ++i)
    {
        int id = shared->warehouse.robots[i].id;
        char wake_name[WH_PROCESS_NAME_SIZE];
        char done_name[WH_PROCESS_NAME_SIZE];
        if (wh_process_name(wake_name, sizeof(wake_name), run_id, "wake", id) != 0 ||
            wh_process_name(done_name, sizeof(done_name), run_id, "done", id) != 0 ||
            wh_process_sem_create(&sems[i * 2], wake_name, 0U,
                                  named_semaphores, &shared->unnamed[i * 2]) != 0 ||
            wh_process_sem_create(&sems[i * 2 + 1], done_name, 0U,
                                  named_semaphores,
                                  &shared->unnamed[i * 2 + 1]) != 0)
        {
            perror("worker semaphore");
            goto cleanup;
        }
        wake[i] = sems[i * 2].handle;
        done[i] = sems[i * 2 + 1].handle;
    }
    for (int i = 0; i < count; ++i)
    {
        workers[i] = fork();
        if (workers[i] < 0)
        {
            perror("fork");
            goto cleanup;
        }
        if (workers[i] == 0)
        {
            wh_process_worker(shared, i, wake[i], done[i], locks[0].handle,
                              locks[1].handle, shared->warehouse.log_fd);
            _exit(0);
        }
    }
    wh_output_bind(&shared->warehouse, warehouse->output_fd, warehouse->log_fd,
                   warehouse->quiet, locks[1].handle);
    status = wh_process_run(&shared->warehouse, max_ticks, delay_ms, wake, done,
                            count, locks[0].handle);
    wh_output_unbind();

cleanup:
    if (shared != NULL)
    {
        if (stop_workers(shared, wake, workers, count, locks[0].handle) != 0)
        {
            status = 1;
        }
        warehouse->tick = shared->warehouse.tick;
        warehouse->stats_assigned = shared->warehouse.stats_assigned;
        warehouse->stats_completed = shared->warehouse.stats_completed;
        warehouse->stats_unreachable = shared->warehouse.stats_unreachable;
        warehouse->stats_moves = shared->warehouse.stats_moves;
        warehouse->stats_waits = shared->warehouse.stats_waits;
    }
    for (int i = 0; i < count * 2; ++i)
    {
        if (wh_process_sem_close(&sems[i]) != 0)
        {
            status = 1;
        }
    }
    for (int i = 0; i < 2; ++i)
    {
        if (wh_process_sem_close(&locks[i]) != 0)
        {
            status = 1;
        }
    }
    if (shared != NULL && wh_process_shm_close(shm_fd, shared) != 0)
    {
        status = 1;
    }
    if (wh_process_shm_unlink(run_id) != 0)
    {
        status = 1;
    }
    return status;
}
