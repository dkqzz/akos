#include "warehouse.h"

#include <stdbool.h>

/* map rules stay here */

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

bool wh_find_route(const Warehouse *warehouse, WhPosition start,
                   WhPosition goal, WhPosition *route, int *route_len)
{
    int row_change[4] = {-1, 1, 0, 0};
    int col_change[4] = {0, 0, -1, 1};
    bool visited[WH_MAX_HEIGHT][WH_MAX_WIDTH] = {false};
    WhPosition previous[WH_MAX_HEIGHT][WH_MAX_WIDTH];
    WhPosition queue[WH_MAX_ROUTE];
    int head = 0;
    int tail = 0;

    if (!in_bounds(warehouse, start) || !in_bounds(warehouse, goal) ||
        blocked(warehouse, start) || blocked(warehouse, goal) ||
        route == NULL || route_len == NULL)
    {
        return false;
    }

    visited[start.row][start.col] = true;
    previous[start.row][start.col] = (WhPosition){-1, -1};
    queue[tail] = start;
    tail++;

    while (head < tail)
    {
        WhPosition current = queue[head];
        head++;
        if (current.row == goal.row && current.col == goal.col)
        {
            break;
        }
        for (int direction = 0; direction < 4; direction++)
        {
            WhPosition next = {current.row + row_change[direction],
                               current.col + col_change[direction]};
            if (!in_bounds(warehouse, next) || visited[next.row][next.col] ||
                blocked(warehouse, next))
            {
                continue;
            }
            visited[next.row][next.col] = true;
            previous[next.row][next.col] = current;
            queue[tail] = next;
            tail++;
        }
    }

    if (!visited[goal.row][goal.col])
    {
        return false;
    }

    WhPosition reverse[WH_MAX_ROUTE];
    int length = 0;
    WhPosition current = goal;
    while (current.row >= 0 && length < WH_MAX_ROUTE)
    {
        reverse[length] = current;
        length++;
        current = previous[current.row][current.col];
    }

    for (int index = 0; index < length; index++)
    {
        route[index] = reverse[length - index - 1];
    }
    *route_len = length;
    return true;
}

bool wh_next_step(const Warehouse *warehouse, const WhRobot *robot,
                  WhPosition *next)
{
    if (warehouse == NULL || robot == NULL || next == NULL ||
        robot->route_index < 0 || robot->route_index >= robot->route_len)
    {
        return false;
    }
    *next = robot->route[robot->route_index];
    return in_bounds(warehouse, *next);
}
