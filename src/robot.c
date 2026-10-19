#include "warehouse.h"

#include <string.h>

/* robot creation and position checks stay here */

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

static int robot_at(const Warehouse *warehouse, WhPosition position)
{
    for (int index = 0; index < warehouse->robot_count; index++)
    {
        const WhRobot *robot = &warehouse->robots[index];
        if (robot->position.row == position.row &&
            robot->position.col == position.col)
        {
            return index;
        }
    }
    return -1;
}

bool wh_set_cell(Warehouse *warehouse, int row, int col, WhCell cell)
{
    WhPosition position = {row, col};
    if (!in_bounds(warehouse, position))
    {
        return false;
    }
    warehouse->cells[row][col] = cell;
    if (cell != WH_NARROW)
    {
        warehouse->narrow_owner[row][col] = -1;
    }
    return true;
}

int wh_add_robot(Warehouse *warehouse, int id, int row, int col,
                 int capacity, int energy, int max_energy)
{
    WhPosition position = {row, col};
    if (warehouse->robot_count >= WH_MAX_ROBOTS || !in_bounds(warehouse, position) ||
        blocked(warehouse, position) || robot_at(warehouse, position) >= 0 ||
        capacity < 1 || energy < 0 || max_energy < 0)
    {
        return -1;
    }
    if (energy > max_energy)
    {
        energy = max_energy;
    }

    int index = warehouse->robot_count;
    warehouse->robot_count++;
    WhRobot *robot = &warehouse->robots[index];
    memset(robot, 0, sizeof(*robot));
    robot->id = id;
    robot->position = position;
    robot->capacity = capacity;
    robot->energy = energy;
    robot->max_energy = max_energy;
    robot->task_id = -1;
    robot->state = WH_ROBOT_IDLE;
    return index;
}
