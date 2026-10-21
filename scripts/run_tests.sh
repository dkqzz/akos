#!/bin/sh
set -e

./scripts/build.sh
ctest --test-dir build --output-on-failure

for executable in warehouse_sim warehouse_sim_cond; do
    for config in data/demo.cfg data/conflict.cfg data/energy.cfg data/impossible.cfg; do
        log_file="/tmp/warehouse-test-${executable}.log"
        "./build/$executable" --config "$config" --log "$log_file" --quiet
        grep '^SUMMARY ' "$log_file" > /dev/null
    done
done

for executable in warehouse_sim warehouse_sim_cond; do
    interrupt_log="/tmp/warehouse-test-interrupted-${executable}.log"
    "./build/$executable" --config data/demo.cfg --log "$interrupt_log" --delay 20 --quiet &
    simulation_pid=$!
    sleep 0.05
    kill -INT "$simulation_pid" 2>/dev/null || true
    wait "$simulation_pid"
    grep '^SUMMARY reason=interrupted ' "$interrupt_log" > /dev/null
done

echo "scenario and interruption checks passed"
