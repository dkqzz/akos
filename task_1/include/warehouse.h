#ifndef WAREHOUSE_H
#define WAREHOUSE_H

#include <stdbool.h>
#include <signal.h>
#include <stddef.h>

#define WH_MAX_WIDTH 32
#define WH_MAX_HEIGHT 32
#define WH_MAX_ROBOTS 32
#define WH_MAX_TASKS 128
#define WH_MAX_ROUTE (WH_MAX_WIDTH * WH_MAX_HEIGHT)

typedef struct
{
    int row;
    int col;
} WhPosition;

typedef enum
{
    WH_FLOOR = '.',
    WH_OBSTACLE = '#',
    WH_PICKUP = 'P',
    WH_DROPOFF = 'D',
    WH_CHARGE = 'C',
    WH_NARROW = 'N'
} WhCell;

typedef enum
{
    WH_TASK_PENDING,
    WH_TASK_TO_PICKUP,
    WH_TASK_LOADING,
    WH_TASK_TO_DROPOFF,
    WH_TASK_UNLOADING,
    WH_TASK_COMPLETED,
    WH_TASK_UNREACHABLE
} WhTaskStatus;

typedef enum
{
    WH_ROBOT_IDLE,
    WH_ROBOT_TO_PICKUP,
    WH_ROBOT_LOADING,
    WH_ROBOT_TO_DROPOFF,
    WH_ROBOT_UNLOADING,
    WH_ROBOT_CHARGING
} WhRobotState;

typedef struct
{
    int id;
    WhPosition pickup;
    WhPosition dropoff;
    int weight;
    WhTaskStatus status;
    int assigned_robot;
    int created_tick;
    int completed_tick;
} WhTask;

typedef struct
{
    int id;
    WhPosition position;
    int capacity;
    int energy;
    int max_energy;
    int task_id;
    WhRobotState state;
    int operation_left;
    int wait_ticks;
    int distance;
    bool carrying;
    WhPosition route[WH_MAX_ROUTE];
    int route_len;
    int route_index;
} WhRobot;

typedef struct
{
    int width;
    int height;
    WhCell cells[WH_MAX_HEIGHT][WH_MAX_WIDTH];
    int robot_count;
    int task_count;
    WhRobot robots[WH_MAX_ROBOTS];
    WhTask tasks[WH_MAX_TASKS];
    int tick;
    int move_energy;
    int charge_rate;
    int load_ticks;
    int unload_ticks;
    int max_wait_ticks;
    int max_ticks;
    int seed;
    int delay_ms;
    int stats_assigned;
    int stats_completed;
    int stats_unreachable;
    int stats_moves;
    int stats_waits;
    int output_fd;
    int log_fd;
    bool quiet;
    bool stop_requested;
    int narrow_owner[WH_MAX_HEIGHT][WH_MAX_WIDTH];
} Warehouse;

extern volatile sig_atomic_t wh_signal_stop;

void wh_init(Warehouse *warehouse, int width, int height);
bool wh_set_cell(Warehouse *warehouse, int row, int col, WhCell cell);
int wh_add_robot(Warehouse *warehouse, int id, int row, int col,
                 int capacity, int energy, int max_energy);
int wh_add_task(Warehouse *warehouse, int id, int pickup_row, int pickup_col,
                int dropoff_row, int dropoff_col, int weight);
bool wh_find_route(const Warehouse *warehouse, WhPosition start,
                   WhPosition goal, WhPosition *route, int *route_len);
bool wh_next_step(const Warehouse *warehouse, const WhRobot *robot,
                  WhPosition *next);
bool wh_validate_invariants(const Warehouse *warehouse, char *message,
                            size_t message_size);
int wh_step(Warehouse *warehouse);
int wh_run(Warehouse *warehouse, int max_ticks, int delay_ms);
bool wh_all_done(const Warehouse *warehouse);
void wh_request_stop(Warehouse *warehouse);
void emit_event(Warehouse *warehouse, const char *format, ...);
void wh_print_map(const Warehouse *warehouse);

#endif
