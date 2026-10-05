#include "warehouse.h"

#include <string.h>

/* task creation stays separate from robot and map code */

static bool in_bounds(const Warehouse *warehouse, WhPosition position)
{
    return position.row >= 0 && position.row < warehouse->height &&
           position.col >= 0 && position.col < warehouse->width;
}

static bool blocked(const Warehouse *warehouse, WhPosition position)
{
    return !in_bounds(warehouse, position) ||
           warehouse->cells[position.row][position.col] == WH_OBSTACLE;
}

int wh_add_task(Warehouse *warehouse, int id, int pickup_row, int pickup_col,
                int dropoff_row, int dropoff_col, int weight)
{
    WhPosition pickup = {pickup_row, pickup_col};
    WhPosition dropoff = {dropoff_row, dropoff_col};
    if (warehouse->task_count >= WH_MAX_TASKS || !in_bounds(warehouse, pickup) ||
        !in_bounds(warehouse, dropoff) || blocked(warehouse, pickup) ||
        blocked(warehouse, dropoff) || weight < 1)
    {
        return -1;
    }

    int index = warehouse->task_count;
    warehouse->task_count++;
    WhTask *task = &warehouse->tasks[index];
    memset(task, 0, sizeof(*task));
    task->id = id;
    task->pickup = pickup;
    task->dropoff = dropoff;
    task->weight = weight;
    task->status = WH_TASK_PENDING;
    task->assigned_robot = -1;
    task->completed_tick = -1;
    return index;
}
