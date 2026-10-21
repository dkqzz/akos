#include "warehouse.h"

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
    sem_t *handle;
} WhSemaphore;

typedef struct
{
    Warehouse *warehouse;
    pthread_t workers[WH_MAX_ROBOTS];
    bool worker_started[WH_MAX_ROBOTS];
    WhSemaphore wake[WH_MAX_ROBOTS];
    WhSemaphore done[WH_MAX_ROBOTS];
    int robot_count;
    bool shutting_down;
} SemaphoreRuntime;

typedef struct
{
    SemaphoreRuntime *runtime;
    int robot_index;
} RobotThreadArgument;

static int semaphore_init(WhSemaphore *semaphore, const char *name)
{
    sem_t *handle = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
    if (handle == SEM_FAILED)
    {
        return -1;
    }
    (void)sem_unlink(name);
    semaphore->handle = handle;
    return 0;
}

static void semaphore_destroy(WhSemaphore *semaphore)
{
    if (semaphore->handle == NULL)
    {
        return;
    }
    (void)sem_close(semaphore->handle);
    semaphore->handle = NULL;
}

static int semaphore_wait(WhSemaphore *semaphore)
{
    int result;
    do
    {
        result = sem_wait(semaphore->handle);
    } while (result != 0 && errno == EINTR);
    return result;
}

static int semaphore_post(WhSemaphore *semaphore)
{
    return sem_post(semaphore->handle);
}

static bool runtime_should_stop(const SemaphoreRuntime *runtime)
{
    const Warehouse *warehouse = runtime->warehouse;
    return runtime->shutting_down || warehouse->stop_requested ||
           wh_signal_stop != 0;
}

static void *robot_worker(void *argument)
{
    RobotThreadArgument *thread_argument = (RobotThreadArgument *)argument;
    SemaphoreRuntime *runtime = thread_argument->runtime;
    Warehouse *warehouse = runtime->warehouse;
    int robot_index = thread_argument->robot_index;

    for (;;)
    {
        if (semaphore_wait(&runtime->wake[robot_index]) != 0)
        {
            break;
        }

        pthread_mutex_lock(&warehouse->state_mutex);
        bool stop = runtime_should_stop(runtime);
        if (!stop)
        {
            wh_robot_step_locked(warehouse, robot_index);
        }
        pthread_mutex_unlock(&warehouse->state_mutex);
        (void)semaphore_post(&runtime->done[robot_index]);
        if (stop)
        {
            break;
        }
    }
    return NULL;
}

static void wake_workers(SemaphoreRuntime *runtime)
{
    for (int index = 0; index < runtime->robot_count; ++index)
    {
        if (runtime->worker_started[index])
        {
            (void)semaphore_post(&runtime->wake[index]);
        }
    }
}

static void wait_for_workers(SemaphoreRuntime *runtime)
{
    for (int index = 0; index < runtime->robot_count; ++index)
    {
        if (runtime->worker_started[index])
        {
            (void)semaphore_wait(&runtime->done[index]);
        }
    }
}

static void stop_workers(SemaphoreRuntime *runtime)
{
    Warehouse *warehouse = runtime->warehouse;
    pthread_mutex_lock(&warehouse->state_mutex);
    runtime->shutting_down = true;
    pthread_mutex_unlock(&warehouse->state_mutex);

    wake_workers(runtime);
    wait_for_workers(runtime);
    for (int index = 0; index < runtime->robot_count; ++index)
    {
        if (runtime->worker_started[index])
        {
            (void)pthread_join(runtime->workers[index], NULL);
        }
    }
}

static void destroy_runtime(SemaphoreRuntime *runtime)
{
    for (int index = 0; index < runtime->robot_count; ++index)
    {
        semaphore_destroy(&runtime->wake[index]);
        semaphore_destroy(&runtime->done[index]);
    }
    wh_runtime_destroy(runtime->warehouse);
}

static int initialize_runtime(SemaphoreRuntime *runtime, Warehouse *warehouse,
                              RobotThreadArgument *arguments)
{
    memset(runtime, 0, sizeof(*runtime));
    runtime->warehouse = warehouse;
    runtime->robot_count = warehouse->robot_count;
    if (wh_runtime_init(warehouse) != 0)
    {
        return -1;
    }

    for (int index = 0; index < runtime->robot_count; ++index)
    {
        char wake_name[64];
        char done_name[64];
        (void)snprintf(wake_name, sizeof(wake_name), "/wh-wake-%ld-%p-%d",
                       (long)getpid(), (void *)runtime, index);
        (void)snprintf(done_name, sizeof(done_name), "/wh-done-%ld-%p-%d",
                       (long)getpid(), (void *)runtime, index);
        if (semaphore_init(&runtime->wake[index], wake_name) != 0 ||
            semaphore_init(&runtime->done[index], done_name) != 0)
        {
            stop_workers(runtime);
            destroy_runtime(runtime);
            return -1;
        }
        arguments[index] = (RobotThreadArgument){runtime, index};
        if (pthread_create(&runtime->workers[index], NULL, robot_worker,
                           &arguments[index]) != 0)
        {
            runtime->shutting_down = true;
            for (int previous = 0; previous < index; ++previous)
            {
                runtime->worker_started[previous] = true;
            }
            stop_workers(runtime);
            destroy_runtime(runtime);
            return -1;
        }
        runtime->worker_started[index] = true;
    }
    return 0;
}

static int emit_run_summary(Warehouse *warehouse, int status, int max_ticks)
{
    const char *reason;
    pthread_mutex_lock(&warehouse->state_mutex);
    if (status != 0)
    {
        reason = "invariant violation";
    }
    else if (wh_signal_stop != 0 || warehouse->stop_requested)
    {
        reason = "interrupted";
    }
    else if (wh_all_done(warehouse))
    {
        reason = "all tasks resolved";
    }
    else if (warehouse->tick >= max_ticks)
    {
        reason = "tick limit";
    }
    else
    {
        reason = "stopped";
    }
    int tick = warehouse->tick;
    int assigned = warehouse->stats_assigned;
    int completed = warehouse->stats_completed;
    int unreachable = warehouse->stats_unreachable;
    int moves = warehouse->stats_moves;
    int waits = warehouse->stats_waits;
    pthread_mutex_unlock(&warehouse->state_mutex);

    emit_event(warehouse,
               "SUMMARY reason=%s ticks=%d assigned=%d completed=%d unreachable=%d moves=%d waits=%d\n",
               reason, tick, assigned, completed, unreachable, moves, waits);
    return status;
}

int wh_run(Warehouse *warehouse, int max_ticks, int delay_ms)
{
    if (max_ticks <= 0)
    {
        max_ticks = warehouse->max_ticks;
    }
    wh_signal_stop = 0;

    SemaphoreRuntime runtime;
    RobotThreadArgument arguments[WH_MAX_ROBOTS];
    if (initialize_runtime(&runtime, warehouse, arguments) != 0)
    {
        return 1;
    }

    int status = 0;
    for (;;)
    {
        pthread_mutex_lock(&warehouse->state_mutex);
        char initial_message[160];
        if (!wh_validate_invariants(warehouse, initial_message,
                                    sizeof(initial_message)))
        {
            emit_event(warehouse, "[tick %d] invariant violation: %s\n",
                       warehouse->tick, initial_message);
            status = 1;
            warehouse->stop_requested = true;
        }
        bool stop = warehouse->stop_requested || wh_signal_stop != 0;
        bool done = wh_all_done(warehouse);
        bool limit = warehouse->tick >= max_ticks;
        if (status == 0 && !stop && !done && !limit)
        {
            wh_assign_pending_tasks(warehouse);
            done = wh_all_done(warehouse);
        }
        pthread_mutex_unlock(&warehouse->state_mutex);

        if (stop || done || limit)
        {
            break;
        }

        wake_workers(&runtime);
        wait_for_workers(&runtime);

        pthread_mutex_lock(&warehouse->state_mutex);
        warehouse->tick++;
        char message[160];
        if (!wh_validate_invariants(warehouse, message, sizeof(message)))
        {
            emit_event(warehouse, "[tick %d] invariant violation: %s\n",
                       warehouse->tick, message);
            status = 1;
            warehouse->stop_requested = true;
        }
        bool tick_done = wh_all_done(warehouse);
        pthread_mutex_unlock(&warehouse->state_mutex);

        if (!warehouse->quiet && warehouse->output_fd >= 0)
        {
            emit_event(warehouse, "MAP tick %d\n", warehouse->tick);
            wh_print_map(warehouse);
        }
        if (status != 0 || tick_done || wh_signal_stop != 0)
        {
            break;
        }
        if (delay_ms > 0)
        {
            usleep((useconds_t)delay_ms * 1000U);
        }
    }

    stop_workers(&runtime);
    int result = emit_run_summary(warehouse, status, max_ticks);
    destroy_runtime(&runtime);
    return result;
}
