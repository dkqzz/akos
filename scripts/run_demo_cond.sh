#!/bin/sh
set -e

./scripts/build.sh
./build/warehouse_sim_cond --config data/demo.cfg --log demo-cond.log --delay 0 "$@"
