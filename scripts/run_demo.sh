#!/bin/sh
set -e

./scripts/build.sh
./build/warehouse_sim --config data/demo.cfg --log demo.log --delay 0 "$@"
