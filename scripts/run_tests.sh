#!/bin/sh
set -e

# run this script from the project directory
./scripts/build.sh
ctest --test-dir build --output-on-failure

for config in data/demo.cfg data/conflict.cfg data/energy.cfg data/impossible.cfg; do
    log_file="/tmp/warehouse-test.log"
    ./build/warehouse_sim --config "$config" --log "$log_file" --quiet
    grep '^SUMMARY ' "$log_file" > /dev/null
done

echo "scenario checks passed"
