#include "warehouse.h"
#include "config.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static Warehouse make_open_warehouse(void)
{
    Warehouse warehouse;
    wh_init(&warehouse, 5, 5);
    warehouse.output_fd = -1;
    warehouse.log_fd = -1;
    warehouse.move_energy = 1;
    warehouse.charge_rate = 3;
    warehouse.load_ticks = 1;
    warehouse.unload_ticks = 1;
    warehouse.max_ticks = 100;
    return warehouse;
}

static void test_route_avoids_obstacle(void)
{
    Warehouse warehouse = make_open_warehouse();
    for (int col = 0; col < 4; ++col)
    {
        assert(wh_set_cell(&warehouse, 2, col, WH_OBSTACLE));
    }

    WhPosition route[WH_MAX_ROUTE];
    int route_len = 0;
    assert(wh_find_route(&warehouse, (WhPosition){0, 0},
                         (WhPosition){4, 4}, route, &route_len));
    assert(route_len > 0);
    for (int i = 0; i < route_len; ++i)
    {
        assert(warehouse.cells[route[i].row][route[i].col] != WH_OBSTACLE);
    }
}

static void test_assignment_and_delivery(void)
{
    Warehouse warehouse = make_open_warehouse();
    wh_set_cell(&warehouse, 0, 0, WH_PICKUP);
    wh_set_cell(&warehouse, 4, 4, WH_DROPOFF);
    assert(wh_add_robot(&warehouse, 1, 0, 1, 10, 50, 50) == 0);
    assert(wh_add_task(&warehouse, 7, 0, 0, 4, 4, 5) == 0);

    assert(wh_run(&warehouse, 100, 0) == 0);
    assert(warehouse.tasks[0].status == WH_TASK_COMPLETED);
    assert(warehouse.stats_completed == 1);
    assert(warehouse.robots[0].task_id == -1);
}

static void test_heavy_task_is_unreachable(void)
{
    Warehouse warehouse = make_open_warehouse();
    assert(wh_add_robot(&warehouse, 1, 0, 0, 4, 50, 50) == 0);
    assert(wh_add_task(&warehouse, 1, 0, 1, 4, 4, 5) == 0);
    assert(wh_run(&warehouse, 20, 0) == 0);
    assert(warehouse.tasks[0].status == WH_TASK_UNREACHABLE);
    assert(warehouse.stats_unreachable == 1);
}

static void test_energy_never_becomes_negative(void)
{
    Warehouse warehouse = make_open_warehouse();
    assert(wh_add_robot(&warehouse, 1, 0, 0, 10, 0, 10) == 0);
    assert(wh_add_task(&warehouse, 1, 0, 1, 4, 4, 1) == 0);
    for (int i = 0; i < 10; ++i)
    {
        wh_step(&warehouse);
        assert(warehouse.robots[0].energy >= 0);
    }
}

static void test_conflicting_robots_do_not_collide(void)
{
    Warehouse warehouse = make_open_warehouse();
    assert(wh_add_robot(&warehouse, 1, 2, 1, 10, 50, 50) == 0);
    assert(wh_add_robot(&warehouse, 2, 2, 3, 10, 50, 50) == 1);
    assert(wh_add_task(&warehouse, 1, 2, 4, 0, 4, 1) == 0);
    assert(wh_add_task(&warehouse, 2, 2, 0, 4, 0, 1) == 1);

    for (int i = 0; i < 20; ++i)
    {
        wh_step(&warehouse);
        assert(!(warehouse.robots[0].position.row == warehouse.robots[1].position.row &&
                 warehouse.robots[0].position.col == warehouse.robots[1].position.col));
    }
}

static void test_configuration_is_loaded(void)
{
    Warehouse warehouse = make_open_warehouse();
    assert(wh_load_config(&warehouse, "data/test-config.cfg") == 0);
    assert(warehouse.width == 6);
    assert(warehouse.height == 5);
    assert(warehouse.cells[1][2] == WH_OBSTACLE);
    assert(warehouse.cells[2][4] == WH_NARROW);
    assert(warehouse.move_energy == 2);
    assert(warehouse.load_ticks == 2);
    assert(warehouse.unload_ticks == 3);
    assert(warehouse.robot_count == 1);
    assert(warehouse.robots[0].capacity == 12);
    assert(warehouse.task_count == 1);
    assert(warehouse.tasks[0].weight == 8);
}

static void test_robot_charges_before_moving(void)
{
    Warehouse warehouse = make_open_warehouse();
    wh_set_cell(&warehouse, 0, 0, WH_CHARGE);
    wh_set_cell(&warehouse, 0, 2, WH_PICKUP);
    wh_set_cell(&warehouse, 0, 4, WH_DROPOFF);
    warehouse.charge_rate = 5;
    assert(wh_add_robot(&warehouse, 1, 0, 0, 10, 0, 5) == 0);
    assert(wh_add_task(&warehouse, 1, 0, 2, 0, 4, 1) == 0);
    assert(wh_run(&warehouse, 30, 0) == 0);
    assert(warehouse.stats_moves > 0);
    assert(warehouse.tasks[0].status == WH_TASK_COMPLETED);
}

static void test_run_reports_invariant_error(void)
{
    Warehouse warehouse = make_open_warehouse();
    wh_set_cell(&warehouse, 0, 0, WH_OBSTACLE);
    assert(wh_add_robot(&warehouse, 1, 0, 1, 10, 10, 10) == 0);
    assert(wh_add_task(&warehouse, 1, 0, 1, 0, 2, 1) == 0);
    warehouse.robots[0].position = (WhPosition){0, 0};
    assert(wh_run(&warehouse, 5, 0) != 0);
}

static void test_condition_backend_delivers_task(void)
{
    Warehouse warehouse = make_open_warehouse();
    wh_set_cell(&warehouse, 0, 0, WH_PICKUP);
    wh_set_cell(&warehouse, 4, 4, WH_DROPOFF);
    assert(wh_add_robot(&warehouse, 1, 0, 1, 10, 50, 50) == 0);
    assert(wh_add_task(&warehouse, 7, 0, 0, 4, 4, 5) == 0);

    assert(wh_run_cond(&warehouse, 100, 0) == 0);
    assert(warehouse.tasks[0].status == WH_TASK_COMPLETED);
    assert(warehouse.stats_completed == 1);
}

static void test_semaphore_backend_resolves_conflict_scenario(void)
{
    Warehouse warehouse;
    wh_init(&warehouse, 1, 1);
    warehouse.output_fd = -1;
    warehouse.log_fd = -1;
    warehouse.quiet = true;
    assert(wh_load_config(&warehouse, "data/conflict.cfg") == 0);

    assert(wh_run(&warehouse, warehouse.max_ticks, 0) == 0);
    assert(warehouse.stats_completed == 2);
    assert(warehouse.stats_unreachable == 0);
    assert(wh_validate_invariants(&warehouse, NULL, 0));
}

static void test_threaded_log_contains_complete_records(void)
{
    char log_path[] = "/tmp/warehouse-thread-log-XXXXXX";
    int log_fd = mkstemp(log_path);
    assert(log_fd >= 0);
    assert(unlink(log_path) == 0);

    Warehouse warehouse;
    wh_init(&warehouse, 1, 1);
    warehouse.output_fd = -1;
    warehouse.log_fd = log_fd;
    warehouse.quiet = true;
    assert(wh_load_config(&warehouse, "data/demo.cfg") == 0);
    warehouse.output_fd = -1;
    warehouse.log_fd = log_fd;
    warehouse.quiet = true;

    assert(wh_run(&warehouse, warehouse.max_ticks, 0) == 0);
    assert(lseek(log_fd, 0, SEEK_SET) == 0);
    char log[32768];
    ssize_t length = read(log_fd, log, sizeof(log) - 1U);
    assert(length > 0);
    log[length] = '\0';
    bool summary_found = false;
    for (char *line = strtok(log, "\n"); line != NULL;
         line = strtok(NULL, "\n"))
    {
        if (strncmp(line, "SUMMARY ", 8) == 0)
        {
            summary_found = true;
        }
        assert(strncmp(line, "[tick ", 6) == 0 ||
               strncmp(line, "SUMMARY ", 8) == 0);
    }
    assert(summary_found);
    assert(close(log_fd) == 0);
}

int main(void)
{
    test_route_avoids_obstacle();
    test_assignment_and_delivery();
    test_heavy_task_is_unreachable();
    test_energy_never_becomes_negative();
    test_conflicting_robots_do_not_collide();
    test_configuration_is_loaded();
    test_robot_charges_before_moving();
    test_run_reports_invariant_error();
    test_condition_backend_delivers_task();
    test_semaphore_backend_resolves_conflict_scenario();
    test_threaded_log_contains_complete_records();
    puts("warehouse tests passed");
    return 0;
}
