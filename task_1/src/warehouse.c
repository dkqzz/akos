#include "warehouse.h"

#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

typedef struct
{
    int robot_index;
    WhPosition from;
    WhPosition to;
    bool valid;
} MoveRequest;

volatile sig_atomic_t wh_signal_stop = 0;

/* one tick is resolved in one simple sequence */

static bool in_bounds(const Warehouse *warehouse, WhPosition position)
{
    return position.row >= 0 && position.row < warehouse->height &&
           position.col >= 0 && position.col < warehouse->width;
}

static bool same_position(WhPosition left, WhPosition right)
{
    return left.row == right.row && left.col == right.col;
}

static int robot_index_at(const Warehouse *warehouse, WhPosition position)
{
    for (int index = 0; index < warehouse->robot_count; ++index)
    {
        if (same_position(warehouse->robots[index].position, position))
        {
            return index;
        }
    }
    return -1;
}

static int task_index_by_id(const Warehouse *warehouse, int task_id)
{
    for (int index = 0; index < warehouse->task_count; ++index)
    {
        if (warehouse->tasks[index].id == task_id)
        {
            return index;
        }
    }
    return -1;
}

static bool is_blocked_cell(const Warehouse *warehouse, WhPosition position)
{
    return !in_bounds(warehouse, position) ||
           warehouse->cells[position.row][position.col] == WH_OBSTACLE;
}

static bool route_exists(const Warehouse *warehouse, WhPosition start,
                         WhPosition goal)
{
    WhPosition route[WH_MAX_ROUTE];
    int route_len = 0;
    return wh_find_route(warehouse, start, goal, route, &route_len);
}

static bool plan_robot_route(Warehouse *warehouse, WhRobot *robot,
                             WhPosition goal)
{
    int route_len = 0;
    if (!wh_find_route(warehouse, robot->position, goal, robot->route,
                       &route_len))
    {
        robot->route_len = 0;
        robot->route_index = 0;
        return false;
    }
    robot->route_len = route_len;
    robot->route_index = route_len > 1 ? 1 : route_len;
    return true;
}

static bool task_has_compatible_robot(const Warehouse *warehouse,
                                      const WhTask *task)
{
    for (int index = 0; index < warehouse->robot_count; ++index)
    {
        const WhRobot *robot = &warehouse->robots[index];
        if (robot->capacity >= task->weight &&
            route_exists(warehouse, robot->position, task->pickup) &&
            route_exists(warehouse, task->pickup, task->dropoff))
        {
            return true;
        }
    }
    return false;
}

static void mark_unreachable(Warehouse *warehouse, WhTask *task)
{
    task->status = WH_TASK_UNREACHABLE;
    task->assigned_robot = -1;
    warehouse->stats_unreachable++;
    emit_event(warehouse, "[tick %d] task %d rejected: no compatible route or capacity\n",
               warehouse->tick, task->id);
}

static void assign_pending_tasks(Warehouse *warehouse)
{
    for (int task_index = 0; task_index < warehouse->task_count; ++task_index)
    {
        WhTask *task = &warehouse->tasks[task_index];
        if (task->status != WH_TASK_PENDING)
        {
            continue;
        }

        /* choose the shortest route, then the smaller id */
        int selected_robot = -1;
        int selected_route_len = WH_MAX_ROUTE + 1;
        for (int robot_index = 0; robot_index < warehouse->robot_count;
             ++robot_index)
        {
            WhRobot *robot = &warehouse->robots[robot_index];
            if (robot->state != WH_ROBOT_IDLE || robot->task_id != -1 ||
                robot->capacity < task->weight)
            {
                continue;
            }
            WhPosition route[WH_MAX_ROUTE];
            int route_len = 0;
            if (!wh_find_route(warehouse, robot->position, task->pickup, route,
                               &route_len) ||
                !route_exists(warehouse, task->pickup, task->dropoff))
            {
                continue;
            }
            if (route_len < selected_route_len ||
                (route_len == selected_route_len &&
                 (selected_robot < 0 || robot->id <
                                            warehouse->robots[selected_robot].id)))
            {
                selected_robot = robot_index;
                selected_route_len = route_len;
            }
        }

        if (selected_robot < 0)
        {
            if (!task_has_compatible_robot(warehouse, task))
            {
                mark_unreachable(warehouse, task);
            }
            continue;
        }

        WhRobot *robot = &warehouse->robots[selected_robot];
        robot->task_id = task->id;
        robot->state = WH_ROBOT_TO_PICKUP;
        robot->wait_ticks = 0;
        task->status = WH_TASK_TO_PICKUP;
        task->assigned_robot = robot->id;
        task->created_tick = warehouse->tick;
        plan_robot_route(warehouse, robot, task->pickup);
        warehouse->stats_assigned++;
        emit_event(warehouse, "[tick %d] task %d assigned to robot %d\n",
                   warehouse->tick, task->id, robot->id);
    }
}

void wh_init(Warehouse *warehouse, int width, int height)
{
    memset(warehouse, 0, sizeof(*warehouse));
    if (width < 1)
    {
        width = 1;
    }
    if (height < 1)
    {
        height = 1;
    }
    if (width > WH_MAX_WIDTH)
    {
        width = WH_MAX_WIDTH;
    }
    if (height > WH_MAX_HEIGHT)
    {
        height = WH_MAX_HEIGHT;
    }
    warehouse->width = width;
    warehouse->height = height;
    warehouse->move_energy = 1;
    warehouse->charge_rate = 3;
    warehouse->load_ticks = 1;
    warehouse->unload_ticks = 1;
    warehouse->max_wait_ticks = 10;
    warehouse->max_ticks = 100;
    warehouse->seed = 1;
    warehouse->delay_ms = 0;
    warehouse->output_fd = STDOUT_FILENO;
    warehouse->log_fd = -1;
    for (int row = 0; row < height; ++row)
    {
        for (int col = 0; col < width; ++col)
        {
            warehouse->cells[row][col] = WH_FLOOR;
            warehouse->narrow_owner[row][col] = -1;
        }
    }
    for (int index = 0; index < WH_MAX_ROBOTS; ++index)
    {
        warehouse->robots[index].task_id = -1;
    }
}

static WhPosition robot_goal(const Warehouse *warehouse, const WhRobot *robot)
{
    int task_index = task_index_by_id(warehouse, robot->task_id);
    if (task_index < 0)
    {
        return robot->position;
    }
    const WhTask *task = &warehouse->tasks[task_index];
    return robot->state == WH_ROBOT_TO_PICKUP ? task->pickup : task->dropoff;
}

static void start_operation_if_at_goal(Warehouse *warehouse, WhRobot *robot)
{
    if (robot->task_id < 0)
    {
        return;
    }
    int task_index = task_index_by_id(warehouse, robot->task_id);
    if (task_index < 0)
    {
        return;
    }
    WhTask *task = &warehouse->tasks[task_index];
    if (robot->state == WH_ROBOT_TO_PICKUP &&
        same_position(robot->position, task->pickup))
    {
        robot->state = WH_ROBOT_LOADING;
        task->status = WH_TASK_LOADING;
        robot->operation_left = warehouse->load_ticks;
        emit_event(warehouse, "[tick %d] robot %d reached pickup for task %d\n",
                   warehouse->tick, robot->id, task->id);
    }
    else if (robot->state == WH_ROBOT_TO_DROPOFF &&
             same_position(robot->position, task->dropoff))
    {
        robot->state = WH_ROBOT_UNLOADING;
        task->status = WH_TASK_UNLOADING;
        robot->operation_left = warehouse->unload_ticks;
        emit_event(warehouse, "[tick %d] robot %d reached dropoff for task %d\n",
                   warehouse->tick, robot->id, task->id);
    }
}

static void update_operation(Warehouse *warehouse, WhRobot *robot)
{
    if (robot->state != WH_ROBOT_LOADING && robot->state != WH_ROBOT_UNLOADING)
    {
        return;
    }
    int task_index = task_index_by_id(warehouse, robot->task_id);
    if (task_index < 0)
    {
        robot->state = WH_ROBOT_IDLE;
        robot->task_id = -1;
        return;
    }
    WhTask *task = &warehouse->tasks[task_index];
    if (robot->operation_left > 0)
    {
        robot->operation_left--;
    }
    if (robot->operation_left > 0)
    {
        return;
    }
    if (robot->state == WH_ROBOT_LOADING)
    {
        robot->carrying = true;
        task->status = WH_TASK_TO_DROPOFF;
        robot->state = WH_ROBOT_TO_DROPOFF;
        robot->wait_ticks = 0;
        plan_robot_route(warehouse, robot, task->dropoff);
        emit_event(warehouse, "[tick %d] robot %d picked up task %d (weight %d)\n",
                   warehouse->tick, robot->id, task->id, task->weight);
    }
    else
    {
        robot->carrying = false;
        task->status = WH_TASK_COMPLETED;
        task->completed_tick = warehouse->tick;
        task->assigned_robot = robot->id;
        robot->state = WH_ROBOT_IDLE;
        robot->task_id = -1;
        robot->route_len = 0;
        robot->route_index = 0;
        warehouse->stats_completed++;
        emit_event(warehouse, "[tick %d] task %d delivered by robot %d\n",
                   warehouse->tick, task->id, robot->id);
    }
}

static void charge_idle_robot(Warehouse *warehouse, WhRobot *robot)
{
    if (robot->state != WH_ROBOT_IDLE ||
        warehouse->cells[robot->position.row][robot->position.col] != WH_CHARGE ||
        robot->energy >= robot->max_energy)
    {
        return;
    }
    int old_energy = robot->energy;
    robot->energy += warehouse->charge_rate;
    if (robot->energy > robot->max_energy)
    {
        robot->energy = robot->max_energy;
    }
    emit_event(warehouse, "[tick %d] robot %d charged %d -> %d\n", warehouse->tick,
               robot->id, old_energy, robot->energy);
}

static bool target_already_accepted(const MoveRequest *requests, int count,
                                    WhPosition target)
{
    for (int index = 0; index < count; ++index)
    {
        if (requests[index].valid && same_position(requests[index].to, target))
        {
            return true;
        }
    }
    return false;
}

static void sort_requests(MoveRequest *requests, int count,
                          const Warehouse *warehouse)
{
    for (int left = 0; left < count; ++left)
    {
        for (int right = left + 1; right < count; ++right)
        {
            const WhRobot *a = &warehouse->robots[requests[left].robot_index];
            const WhRobot *b = &warehouse->robots[requests[right].robot_index];
            if (b->wait_ticks > a->wait_ticks ||
                (b->wait_ticks == a->wait_ticks && b->id < a->id))
            {
                MoveRequest temp = requests[left];
                requests[left] = requests[right];
                requests[right] = temp;
            }
        }
    }
}

static void process_moves(Warehouse *warehouse)
{
    MoveRequest requests[WH_MAX_ROBOTS];
    int request_count = 0;
    for (int index = 0; index < warehouse->robot_count; ++index)
    {
        WhRobot *robot = &warehouse->robots[index];
        if (robot->state != WH_ROBOT_TO_PICKUP &&
            robot->state != WH_ROBOT_TO_DROPOFF)
        {
            continue;
        }
        start_operation_if_at_goal(warehouse, robot);
        if (robot->state != WH_ROBOT_TO_PICKUP &&
            robot->state != WH_ROBOT_TO_DROPOFF)
        {
            continue;
        }
        WhPosition goal = robot_goal(warehouse, robot);
        if (robot->route_len == 0 || robot->route_index >= robot->route_len ||
            !same_position(robot->route[robot->route_len - 1], goal))
        {
            plan_robot_route(warehouse, robot, goal);
        }
        WhPosition next;
        if (!wh_next_step(warehouse, robot, &next))
        {
            continue;
        }
        requests[request_count++] = (MoveRequest){index, robot->position, next, false};
    }
    sort_requests(requests, request_count, warehouse);

    /* remember accepted cells before checking the next request */
    int accepted_count = 0;
    for (int index = 0; index < request_count; ++index)
    {
        MoveRequest *request = &requests[index];
        WhRobot *robot = &warehouse->robots[request->robot_index];
        if (robot->energy < warehouse->move_energy &&
            warehouse->cells[robot->position.row][robot->position.col] == WH_CHARGE)
        {
            int old_energy = robot->energy;
            robot->energy += warehouse->charge_rate;
            if (robot->energy > robot->max_energy)
            {
                robot->energy = robot->max_energy;
            }
            emit_event(warehouse, "[tick %d] robot %d charged %d -> %d\n",
                       warehouse->tick, robot->id, old_energy, robot->energy);
            robot->wait_ticks = 0;
            continue;
        }
        bool allowed = robot->energy >= warehouse->move_energy &&
                       !is_blocked_cell(warehouse, request->to) &&
                       robot_index_at(warehouse, request->to) < 0 &&
                       !target_already_accepted(requests, accepted_count, request->to);
        WhCell target_cell = warehouse->cells[request->to.row][request->to.col];
        if (allowed && target_cell == WH_NARROW &&
            warehouse->narrow_owner[request->to.row][request->to.col] != -1 &&
            warehouse->narrow_owner[request->to.row][request->to.col] != robot->id)
        {
            allowed = false;
        }
        if (allowed)
        {
            WhCell source_cell = warehouse->cells[robot->position.row][robot->position.col];
            robot->position = request->to;
            if (robot->route_index < robot->route_len)
            {
                robot->route_index++;
            }
            robot->energy -= warehouse->move_energy;
            if (robot->energy < 0)
            {
                robot->energy = 0;
            }
            robot->distance++;
            robot->wait_ticks = 0;
            if (target_cell == WH_NARROW)
            {
                warehouse->narrow_owner[request->to.row][request->to.col] = robot->id;
            }
            if (source_cell == WH_NARROW &&
                warehouse->narrow_owner[request->from.row][request->from.col] == robot->id)
            {
                warehouse->narrow_owner[request->from.row][request->from.col] = -1;
            }
            request->valid = true;
            requests[accepted_count++] = *request;
            warehouse->stats_moves++;
            emit_event(warehouse, "[tick %d] robot %d moved (%d,%d)->(%d,%d), energy=%d\n",
                       warehouse->tick, robot->id, request->from.row, request->from.col,
                       request->to.row, request->to.col, robot->energy);
        }
        else
        {
            robot->wait_ticks++;
            warehouse->stats_waits++;
            emit_event(warehouse, "[tick %d] robot %d waits at (%d,%d)\n", warehouse->tick,
                       robot->id, robot->position.row, robot->position.col);
            if (robot->wait_ticks > warehouse->max_wait_ticks)
            {
                robot->wait_ticks = warehouse->max_wait_ticks;
                emit_event(warehouse, "[tick %d] robot %d still blocked; keeps reservation policy\n",
                           warehouse->tick, robot->id);
            }
        }
    }
}

bool wh_validate_invariants(const Warehouse *warehouse, char *message,
                            size_t message_size)
{
    if (message != NULL && message_size > 0)
    {
        message[0] = '\0';
    }
    for (int left = 0; left < warehouse->robot_count; ++left)
    {
        const WhRobot *robot = &warehouse->robots[left];
        if (!in_bounds(warehouse, robot->position) ||
            is_blocked_cell(warehouse, robot->position))
        {
            if (message != NULL && message_size > 0)
            {
                snprintf(message, message_size, "robot %d is on an invalid cell",
                         robot->id);
            }
            return false;
        }
        if (robot->energy < 0)
        {
            if (message != NULL && message_size > 0)
            {
                snprintf(message, message_size, "robot %d has negative energy",
                         robot->id);
            }
            return false;
        }
        for (int right = left + 1; right < warehouse->robot_count; ++right)
        {
            if (same_position(robot->position, warehouse->robots[right].position))
            {
                if (message != NULL && message_size > 0)
                {
                    snprintf(message, message_size,
                             "robots %d and %d occupy one cell", robot->id,
                             warehouse->robots[right].id);
                }
                return false;
            }
        }
    }
    for (int task_index = 0; task_index < warehouse->task_count; ++task_index)
    {
        const WhTask *task = &warehouse->tasks[task_index];
        int occurrences = 0;
        for (int robot_index = 0; robot_index < warehouse->robot_count; ++robot_index)
        {
            if (warehouse->robots[robot_index].task_id == task->id)
            {
                occurrences++;
            }
        }
        if (occurrences > 1)
        {
            if (message != NULL && message_size > 0)
            {
                snprintf(message, message_size, "task %d assigned to multiple robots",
                         task->id);
            }
            return false;
        }
    }
    return true;
}

int wh_step(Warehouse *warehouse)
{
    if (warehouse->stop_requested || wh_all_done(warehouse))
    {
        return 1;
    }
    assign_pending_tasks(warehouse);
    for (int index = 0; index < warehouse->robot_count; ++index)
    {
        update_operation(warehouse, &warehouse->robots[index]);
        charge_idle_robot(warehouse, &warehouse->robots[index]);
    }
    process_moves(warehouse);
    warehouse->tick++;
    char message[160];
    if (!wh_validate_invariants(warehouse, message, sizeof(message)))
    {
        emit_event(warehouse, "[tick %d] invariant violation: %s\n", warehouse->tick,
                   message);
        return -1;
    }
    return wh_all_done(warehouse) ? 1 : 0;
}

bool wh_all_done(const Warehouse *warehouse)
{
    for (int index = 0; index < warehouse->task_count; ++index)
    {
        if (warehouse->tasks[index].status != WH_TASK_COMPLETED &&
            warehouse->tasks[index].status != WH_TASK_UNREACHABLE)
        {
            return false;
        }
    }
    return true;
}

int wh_run(Warehouse *warehouse, int max_ticks, int delay_ms)
{
    if (max_ticks <= 0)
    {
        max_ticks = warehouse->max_ticks;
    }
    int status = 0;
    while (!warehouse->stop_requested && wh_signal_stop == 0 && !wh_all_done(warehouse) &&
           warehouse->tick < max_ticks)
    {
        int result = wh_step(warehouse);
        if (result < 0)
        {
            status = 1;
            break;
        }
        if (!warehouse->quiet && warehouse->output_fd >= 0)
        {
            dprintf(warehouse->output_fd, "MAP tick %d\n", warehouse->tick);
            wh_print_map(warehouse);
        }
        if (delay_ms > 0)
        {
            usleep((useconds_t)delay_ms * 1000U);
        }
    }
    if (wh_signal_stop != 0)
    {
        warehouse->stop_requested = true;
    }
    const char *reason = status != 0 ? "invariant violation" : (warehouse->stop_requested ? "interrupted" : (wh_all_done(warehouse) ? "all tasks resolved" : "tick limit"));
    emit_event(warehouse,
               "SUMMARY reason=%s ticks=%d assigned=%d completed=%d unreachable=%d moves=%d waits=%d\n",
               reason, warehouse->tick, warehouse->stats_assigned,
               warehouse->stats_completed, warehouse->stats_unreachable,
               warehouse->stats_moves, warehouse->stats_waits);
    return status;
}

void wh_request_stop(Warehouse *warehouse)
{
    warehouse->stop_requested = true;
}
