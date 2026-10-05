cmake_minimum_required(VERSION 3.20)
project(warehouse_robots C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)

include_directories(include)

add_library(warehouse_model src/warehouse.c src/map.c src/robot.c src/task.c src/io.c src/config.c)
target_include_directories(warehouse_model PUBLIC include)

add_executable(warehouse_sim src/main.c)
target_link_libraries(warehouse_sim PRIVATE warehouse_model)

add_executable(warehouse_tests tests/test_warehouse.c)
target_link_libraries(warehouse_tests PRIVATE warehouse_model)
enable_testing()
add_test(NAME warehouse_tests COMMAND warehouse_tests)
set_tests_properties(warehouse_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
