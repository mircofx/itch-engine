#!/usr/bin/env bash
# Diagnostics for the counter matrix: overflow count per ladder width and probe lengths per order table.
# Not a benchmark: binaries are built with -DPROBE_STATS (extra counting in the hot path) and nothing is timed.
# It explains bench_counters.sh's results, it doesn't replace them.
#
# usage: ./results/bench_diag.sh | tee results/diag.txt

set -e

DATA=~/data/itch_2gb.bin
SRC=$(cd "$(dirname "$0")/.." && pwd)
COMMIT=$(git -C "$SRC" rev-parse --short HEAD)
git -C "$SRC" diff --quiet --ignore-cr-at-eol HEAD -- src CMakeLists.txt || COMMIT="$COMMIT+dirty"   # see bench_counters.sh
echo "# start $(date -Iseconds) commit=$COMMIT flags=-DPROBE_STATS"

diag() {    # $1 = name, $2 = TABLE_MODE, $3 = HASH_MODE, $4 = LADDER_WIDTH
  local d="$HOME/itch-bench-builds/diag-$1"
  rm -rf "$d"
  cmake -S "$SRC" -B "$d" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-13 \
    -DITCH_BUILD_TESTS=OFF -DITCH_BUILD_BENCH=OFF \
    -DCMAKE_CXX_FLAGS="-DPROBE_STATS -DBOOK_MODE=1 -DTABLE_MODE=$2 -DHASH_MODE=$3 -DLADDER_WIDTH=$4" > /dev/null 2>&1
  cmake --build "$d" -j > /dev/null 2>&1
  local out ov pr
  out=$("$d/itch_engine" "$DATA")
  ov=$(awk '/^ladder *: missing/ {for (i=1;i<=NF;i++) if ($i ~ /^overflow=/) {sub(/overflow=/,"",$i); print $i}}' <<< "$out")
  pr=$(awk '/^probes/ {sub(/^probes *: */,""); print}' <<< "$out")
  echo "diag $1 overflow=$ov $pr"
}

diag id8192   1 0 8192
diag fib8192  1 1 8192
diag fib4096  1 1 4096
diag fib1024  1 1 1024
diag fib512   1 1 512
diag gid8192  2 0 8192
diag gfib8192 2 1 8192
echo "# end $(date -Iseconds)"
