#!/bin/sh
set -e

# run this script from the project directory
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
