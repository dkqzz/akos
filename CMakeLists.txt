cmake_minimum_required(VERSION 3.20)
project(warehouse_robots C)

find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)

include_directories(include)

add_library(warehouse_model src/warehouse.c src/map.c src/robot.c src/task.c src/io.c src/config.c)
target_include_directories(warehouse_model PUBLIC include)
target_link_libraries(warehouse_model PUBLIC Threads::Threads)

add_executable(warehouse_sim src/main.c src/sim_semaphore.c)
target_link_libraries(warehouse_sim PRIVATE warehouse_model)

add_executable(warehouse_sim_cond src/main.c src/sim_condition.c)
target_compile_definitions(warehouse_sim_cond PRIVATE WH_USE_COND=1)
target_link_libraries(warehouse_sim_cond PRIVATE warehouse_model)

add_executable(warehouse_tests tests/test_warehouse.c src/sim_semaphore.c src/sim_condition.c)
target_link_libraries(warehouse_tests PRIVATE warehouse_model)
enable_testing()
add_test(NAME warehouse_tests COMMAND warehouse_tests)
set_tests_properties(warehouse_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(warehouse_process_runtime
    src/process_ipc.c
    src/process_sem.c
    src/process_sim.c
    src/sim_process_parent.c)
target_include_directories(warehouse_process_runtime PUBLIC include)
target_link_libraries(warehouse_process_runtime PUBLIC warehouse_model)

add_executable(warehouse_sim_proc_named src/main.c)
target_compile_definitions(warehouse_sim_proc_named PRIVATE WH_PROCESS_NAMED=1)
target_link_libraries(warehouse_sim_proc_named PRIVATE warehouse_process_runtime)

add_executable(warehouse_sim_proc_unnamed src/main.c)
target_compile_definitions(warehouse_sim_proc_unnamed PRIVATE WH_PROCESS_UNNAMED=1)
target_link_libraries(warehouse_sim_proc_unnamed PRIVATE warehouse_process_runtime)

add_executable(warehouse_proc_shm_controller src/process_shm_controller.c)
target_link_libraries(warehouse_proc_shm_controller PRIVATE warehouse_process_runtime)

add_executable(warehouse_proc_shm_robot src/process_shm_robot.c)
target_link_libraries(warehouse_proc_shm_robot PRIVATE warehouse_process_runtime)

add_executable(warehouse_process_ipc_tests tests/test_process_ipc.c)
target_link_libraries(warehouse_process_ipc_tests PRIVATE warehouse_process_runtime)
add_test(NAME warehouse_process_ipc_tests COMMAND warehouse_process_ipc_tests)

add_test(NAME warehouse_process_tests
    COMMAND sh scripts/test_process_modes.sh)
set_tests_properties(warehouse_process_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(warehouse_tcp_runtime src/tcp.c)
target_link_libraries(warehouse_tcp_runtime PUBLIC warehouse_model)

add_executable(warehouse_tcp_server src/tcp_server.c)
target_link_libraries(warehouse_tcp_server PRIVATE warehouse_tcp_runtime)

add_executable(warehouse_tcp_robot src/tcp_client.c)
target_compile_definitions(warehouse_tcp_robot PRIVATE WH_TCP_ROBOT=1)
target_link_libraries(warehouse_tcp_robot PRIVATE warehouse_tcp_runtime)

add_executable(warehouse_tcp_observer src/tcp_client.c)
target_link_libraries(warehouse_tcp_observer PRIVATE warehouse_tcp_runtime)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME warehouse_tcp_tests
        COMMAND "${Python3_EXECUTABLE}" scripts/test_tcp.py "$<TARGET_FILE_DIR:warehouse_tcp_server>")
    set_tests_properties(warehouse_tcp_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        SKIP_RETURN_CODE 77
        TIMEOUT 70)
endif()
