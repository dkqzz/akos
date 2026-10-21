#include "warehouse.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
    Warehouse *warehouse;
    pthread_t workers[WH_MAX_ROBOTS];
    bool worker_started[WH_MAX_ROBOTS];
    pthread_cond_t wake[WH_MAX_ROBOTS];
    pthread_cond_t done[WH_MAX_ROBOTS];
    bool wake_initialized[WH_MAX_ROBOTS];
    bool done_initialized[WH_MAX_ROBOTS];
    bool step_pending[WH_MAX_ROBOTS];
    bool step_done[WH_MAX_ROBOTS];
    int robot_count;
    bool shutting_down;
} ConditionRuntime;

typedef struct
{
    ConditionRuntime *runtime;
    int robot_index;
} RobotThreadArgument;

static bool runtime_should_stop(const ConditionRuntime *runtime)
{
    const Warehouse *warehouse = runtime->warehouse;
    return runtime->shutting_down || warehouse->stop_requested ||
           wh_signal_stop != 0;
}

static void *robot_worker(void *argument)
{
    RobotThreadArgument *thread_argument = (RobotThreadArgument *)argument;
    ConditionRuntime *runtime = thread_argument->runtime;
    Warehouse *warehouse = runtime->warehouse;
    int robot_index = thread_argument->robot_index;

    pthread_mutex_lock(&warehouse->state_mutex);
    for (;;)
    {
        while (!runtime->step_pending[robot_index] &&
               !runtime->shutting_down)
        {
            (void)pthread_cond_wait(&runtime->wake[robot_index],
                                    &warehouse->state_mutex);
        }
        if (runtime->shutting_down)
        {
            break;
        }

        runtime->step_pending[robot_index] = false;
        if (!runtime_should_stop(runtime))
        {
            wh_robot_step_locked(warehouse, robot_index);
        }
        runtime->step_done[robot_index] = true;
        (void)pthread_cond_signal(&runtime->done[robot_index]);
    }
    pthread_mutex_unlock(&warehouse->state_mutex);
    return NULL;
}

static void stop_workers(ConditionRuntime *runtime)
{
    Warehouse *warehouse = runtime->warehouse;
    pthread_mutex_lock(&warehouse->state_mutex);
    runtime->shutting_down = true;
    for (int index = 0; index < runtime->robot_count; ++index)
    {
        if (runtime->wake_initialized[index])
        {
            (void)pthread_cond_broadcast(&runtime->wake[index]);
        }
    }
    pthread_mutex_unlock(&warehouse->state_mutex);

    for (int index = 0; index < runtime->robot_count; ++index)
    {
        if (runtime->worker_started[index])
        {
            (void)pthread_join(runtime->workers[index], NULL);
        }
    }
}

static void destroy_runtime(ConditionRuntime *runtime)
{
    for (int index = 0; index < runtime->robot_count; ++index)
    {
        if (runtime->wake_initialized[index])
        {
            (void)pthread_cond_destroy(&runtime->wake[index]);
        }
        if (runtime->done_initialized[index])
        {
            (void)pthread_cond_destroy(&runtime->done[index]);
        }
    }
    wh_runtime_destroy(runtime->warehouse);
}

static int initialize_runtime(ConditionRuntime *runtime, Warehouse *warehouse,
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
        if (pthread_cond_init(&runtime->wake[index], NULL) != 0)
        {
            stop_workers(runtime);
            destroy_runtime(runtime);
            return -1;
        }
        runtime->wake_initialized[index] = true;
        if (pthread_cond_init(&runtime->done[index], NULL) != 0)
        {
            stop_workers(runtime);
            destroy_runtime(runtime);
            return -1;
        }
        runtime->done_initialized[index] = true;
        arguments[index] = (RobotThreadArgument){runtime, index};
        if (pthread_create(&runtime->workers[index], NULL, robot_worker,
                           &arguments[index]) != 0)
        {
            runtime->shutting_down = true;
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

int wh_run_cond(Warehouse *warehouse, int max_ticks, int delay_ms)
{
    if (max_ticks <= 0)
    {
        max_ticks = warehouse->max_ticks;
    }
    wh_signal_stop = 0;

    ConditionRuntime runtime;
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
            if (!done)
            {
                for (int index = 0; index < runtime.robot_count; ++index)
                {
                    runtime.step_done[index] = false;
                    runtime.step_pending[index] = true;
                    (void)pthread_cond_signal(&runtime.wake[index]);
                }
            }
        }
        pthread_mutex_unlock(&warehouse->state_mutex);

        if (stop || done || limit)
        {
            break;
        }

        pthread_mutex_lock(&warehouse->state_mutex);
        for (int index = 0; index < runtime.robot_count; ++index)
        {
            while (!runtime.step_done[index] && !runtime.shutting_down)
            {
                (void)pthread_cond_wait(&runtime.done[index],
                                        &warehouse->state_mutex);
            }
        }
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
