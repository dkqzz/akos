#define _POSIX_C_SOURCE 200809L

#include "process.h"
#include "io.h"

#include <errno.h>
#include <signal.h>
#include <time.h>

int wh_process_wait(sem_t *sem, unsigned int timeout_ms, bool stop_on_signal)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
    {
        return -1;
    }
    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    for (;;)
    {
        if (stop_on_signal && wh_signal_stop != 0)
        {
            return 2;
        }
        if (sem_trywait(sem) == 0)
        {
            return 0;
        }
        if (errno == EINTR)
        {
            continue;
        }
        if (errno != EAGAIN)
        {
            return -1;
        }
        struct timespec now;
        if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        {
            return -1;
        }
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
        {
            return 1;
        }
        const struct timespec pause = {0, 10000000L};
        nanosleep(&pause, NULL);
    }
}

void wh_process_worker(WhProcessShared *shared, int index, sem_t *wake,
                       sem_t *done, sem_t *state_lock, sem_t *output_lock,
                       int log_fd)
{
    Warehouse *warehouse = &shared->warehouse;
    wh_output_bind(warehouse, -1, log_fd, true, output_lock);
    for (;;)
    {
        int ready = wh_process_wait(wake, 1000U, true);
        if (ready == 1)
        {
            continue;
        }
        if (ready != 0)
        {
            break;
        }
        if (wh_process_sem_lock(state_lock) != 0)
        {
            break;
        }
        bool stop = warehouse->stop_requested;
        if (!stop)
        {
            wh_robot_step_locked(warehouse, index);
        }
        sem_post(state_lock);
        if (stop || sem_post(done) != 0)
        {
            break;
        }
    }
    wh_output_unbind();
}

static void delay_ms(int ms)
{
    struct timespec delay = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR &&
           wh_signal_stop == 0)
    {
    }
}

int wh_process_run(Warehouse *warehouse, int max_ticks, int delay,
                   sem_t **wake, sem_t **done, int count,
                   sem_t *state_lock)
{
    int status = 0;
    for (;;)
    {
        if (wh_process_sem_lock(state_lock) != 0)
        {
            status = 1;
            break;
        }
        if (wh_signal_stop != 0)
        {
            warehouse->stop_requested = true;
        }
        char message[160];
        if (!wh_validate_invariants(warehouse, message, sizeof(message)))
        {
            emit_event(warehouse, "[tick %d] invariant violation: %s\n",
                       warehouse->tick, message);
            status = 1;
        }
        bool stopped = warehouse->stop_requested || status != 0;
        bool complete = wh_all_done(warehouse);
        bool limit = warehouse->tick >= max_ticks;
        if (!stopped && !complete && !limit)
        {
            wh_assign_pending_tasks(warehouse);
            complete = wh_all_done(warehouse);
        }
        sem_post(state_lock);
        if (stopped || complete || limit)
        {
            break;
        }
        for (int i = 0; i < count; ++i)
        {
            if (sem_post(wake[i]) != 0)
            {
                status = 1;
            }
        }
        for (int i = 0; status == 0 && i < count; ++i)
        {
            int result = wh_process_wait(done[i], 30000U, true);
            if (result == 2 && wh_signal_stop != 0)
            {
                break;
            }
            if (result != 0)
            {
                status = 1;
            }
        }
        if (status != 0 || wh_signal_stop != 0)
        {
            continue;
        }
        if (wh_process_sem_lock(state_lock) != 0)
        {
            status = 1;
            break;
        }
        warehouse->tick++;
        if (!wh_validate_invariants(warehouse, message, sizeof(message)))
        {
            emit_event(warehouse, "[tick %d] invariant violation: %s\n",
                       warehouse->tick, message);
            status = 1;
        }
        complete = wh_all_done(warehouse);
        sem_post(state_lock);
        if (!warehouse->quiet)
        {
            emit_event(warehouse, "MAP tick %d\n", warehouse->tick);
            wh_print_map(warehouse);
        }
        if (complete || status != 0 || wh_signal_stop != 0)
        {
            break;
        }
        if (delay > 0)
        {
            delay_ms(delay);
        }
    }

    const char *reason = status != 0 ? "invariant violation" : wh_signal_stop != 0  ? "interrupted"
                                                           : wh_all_done(warehouse) ? "all tasks resolved"
                                                                                    : "tick limit";
    emit_event(warehouse,
               "SUMMARY reason=%s ticks=%d assigned=%d completed=%d unreachable=%d moves=%d waits=%d\n",
               reason, warehouse->tick, warehouse->stats_assigned,
               warehouse->stats_completed, warehouse->stats_unreachable,
               warehouse->stats_moves, warehouse->stats_waits);
    return status;
}
