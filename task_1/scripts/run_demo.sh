#!/bin/sh
set -e

# run this script from the project directory
./scripts/build.sh
./build/warehouse_sim --config data/demo.cfg --log demo.log --delay 0 "$@"
