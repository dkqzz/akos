#!/bin/sh
set -eu

temporary_dir=$(mktemp -d /tmp/wh-test.XXXXXX)
trap 'rm -rf "$temporary_dir"' EXIT HUP INT TERM

for mode in named unnamed; do
    ./build/warehouse_sim_proc_$mode --config data/demo.cfg \
        --log "$temporary_dir/$mode.log" --quiet
    if ! grep -q '^SUMMARY reason=all tasks resolved .*completed=3' \
        "$temporary_dir/$mode.log"; then
        printf '%s process mode failed\n' "$mode" >&2
        exit 1
    fi
done

./build/warehouse_sim_proc_named --config data/demo.cfg \
    --log "$temporary_dir/interrupted.log" --delay 1000 --quiet &
process_id=$!
sleep 0.2
kill -TERM "$process_id"
wait "$process_id"
grep -q '^SUMMARY reason=interrupted ' "$temporary_dir/interrupted.log"

run_id="t$$"
./scripts/run_process_shm.sh --config data/demo.cfg \
    --log "$temporary_dir/shm.log" --run-id "$run_id"
grep -q '^SUMMARY reason=all tasks resolved .*completed=3' \
    "$temporary_dir/shm.log"
[ ! -e "/tmp/wh-$run_id-shm--1" ]

run_id="n$$"
if ./build/warehouse_proc_shm_controller --config data/demo.cfg \
    --log "$temporary_dir/missing.log" --run-id "$run_id" --quiet \
    2>"$temporary_dir/missing.err"; then
    printf 'controller should fail when no robots are started\n' >&2
    exit 1
fi
grep -q 'run ./scripts/run_process_shm.sh' "$temporary_dir/missing.err"
[ ! -e "/tmp/wh-$run_id-shm--1" ]
printf 'all process modes passed\n'
