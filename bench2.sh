#!/usr/bin/env bash
# 25 runs each, single-threaded then threaded, with progress and a running ETA.

DATA=~/itch-engine/data/01302019.NASDAQ_ITCH50
MSGS=368366634
N=25

> single_runs.txt
> threaded_runs.txt

echo "=== single-threaded: $N runs ==="
t_start=$(date +%s)
for i in $(seq 1 $N); do
    run_start=$(date +%s.%N)
    rate=$(./build/itch_engine "$DATA" 2>/dev/null | awk '/^rate/ {print $3}')
    run_end=$(date +%s.%N)
    echo "$rate" >> single_runs.txt

    elapsed=$(( $(date +%s) - t_start ))
    avg=$(echo "scale=1; $elapsed / $i" | bc)
    eta=$(echo "scale=0; $avg * ($N - $i) / 60" | bc)
    echo "[single $i/$N] rate=${rate} M msg/s | elapsed=${elapsed}s | ~${eta} min left in this batch"
done

echo "=== threaded: $N runs ==="
t_start=$(date +%s)
for i in $(seq 1 $N); do
    run_start=$(date +%s.%N)
    ./build/itch_engine_threaded "$DATA" > /dev/null 2>&1
    run_end=$(date +%s.%N)
    dt=$(echo "$run_end - $run_start" | bc)
    rate=$(echo "scale=3; $MSGS / $dt / 1000000" | bc)
    echo "$dt $rate" >> threaded_runs.txt

    elapsed=$(( $(date +%s) - t_start ))
    avg=$(echo "scale=1; $elapsed / $i" | bc)
    eta=$(echo "scale=0; $avg * ($N - $i) / 60" | bc)
    echo "[threaded $i/$N] time=${dt}s rate=${rate} M msg/s | elapsed=${elapsed}s | ~${eta} min left in this batch"
done

echo "=== done ==="
echo "single mean:  $(awk '{s+=$1; c++} END{print s/c}' single_runs.txt) M msg/s"
echo "threaded mean: $(awk '{s+=$2; c++} END{print s/c}' threaded_runs.txt) M msg/s"