#!/bin/sh
set -eu

config="data/demo.cfg"
log="warehouse-process-shm.log"
run_id="r$$"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --config|--log|--run-id)
            if [ "$#" -lt 2 ]; then exit 2; fi
            option=$1
            value=$2
            shift 2
            case "$option" in
                --config) config=$value ;;
                --log) log=$value ;;
                --run-id) run_id=$value ;;
            esac
            ;;
        *)
            printf 'usage: %s [--config PATH] [--log PATH] [--run-id ID]\n' "$0" >&2
            exit 2
            ;;
    esac
done

controller="./build/warehouse_proc_shm_controller"
robot="./build/warehouse_proc_shm_robot"
if [ ! -x "$controller" ] || [ ! -x "$robot" ]; then
    printf 'run ./scripts/build.sh first\n' >&2
    exit 2
fi
robot_ids=$(awk -F= '/^[[:space:]]*robot[[:space:]]*=/ { split($2, a, ","); gsub(/[[:space:]]/, "", a[1]); print a[1] }' "$config")

controller_pid=
robot_pids=
controller_done=false
robots_done=false
forward_signal() {
    if [ -n "$controller_pid" ]; then
        kill -INT "$controller_pid" 2>/dev/null || true
    fi
}
cleanup() {
    if [ "$controller_done" = false ] && [ -n "$controller_pid" ]; then
        kill -TERM "$controller_pid" 2>/dev/null || true
    fi
    if [ "$robots_done" = false ]; then
        for pid in $robot_pids; do kill -TERM "$pid" 2>/dev/null || true; done
    fi
}
trap forward_signal INT TERM
trap cleanup EXIT HUP

"$controller" --config "$config" --log "$log" --run-id "$run_id" --quiet &
controller_pid=$!
shm_path="/tmp/wh-$run_id-shm--1"
attempt=0
while [ ! -e "$shm_path" ]; do
    if ! kill -0 "$controller_pid" 2>/dev/null; then
        wait "$controller_pid" || exit $?
    fi
    if [ "$attempt" -ge 100 ]; then
        printf 'controller startup timed out\n' >&2
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 0.05
done
for id in $robot_ids; do
    "$robot" --run-id "$run_id" --robot-id "$id" --log "$log" &
    robot_pids="$robot_pids $!"
done

controller_status=0
while :; do
    if wait "$controller_pid"; then
        controller_status=0
        break
    else
        controller_status=$?
    fi
    if ! kill -0 "$controller_pid" 2>/dev/null; then break; fi
done
controller_done=true
if [ "$controller_status" -ne 0 ]; then
    for pid in $robot_pids; do kill -TERM "$pid" 2>/dev/null || true; done
fi
robot_status=0
for pid in $robot_pids; do
    if ! wait "$pid"; then robot_status=1; fi
done
robots_done=true
if [ "$controller_status" -ne 0 ]; then exit "$controller_status"; fi
exit "$robot_status"
