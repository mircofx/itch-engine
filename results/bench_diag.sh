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

diag() {    # $1 = name, $2 = TABLE_MODE, $3 = HASH_MODE, $4 = LADDER_WIDTH, $5 = LEVEL_BYTES
  local d="$HOME/itch-bench-builds/diag-$1"
  rm -rf "$d"
  cmake -S "$SRC" -B "$d" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-13 \
    -DITCH_BUILD_TESTS=OFF -DITCH_BUILD_BENCH=OFF \
    -DCMAKE_CXX_FLAGS="-DPROBE_STATS -DBOOK_MODE=1 -DTABLE_MODE=$2 -DHASH_MODE=$3 -DLADDER_WIDTH=$4 -DLEVEL_BYTES=$5" > /dev/null 2>&1
  cmake --build "$d" -j > /dev/null 2>&1
  local out ov pr rss
  # /usr/bin/time -f %M: peak resident memory in KB (includes the mapped file's resident pages)
  out=$(/usr/bin/time -f "maxrss_kb %M" "$d/itch_engine" "$DATA" 2>&1)
  rss=$(awk '/^maxrss_kb/ {print $2}' <<< "$out")
  ov=$(awk '/^ladder *: missing/ {for (i=1;i<=NF;i++) if ($i ~ /^overflow=/) {sub(/overflow=/,"",$i); print $i}}' <<< "$out")
  pr=$(awk '/^probes/ {sub(/^probes *: */,""); print}' <<< "$out")
  echo "diag $1 overflow=$ov $pr maxrss_kb=$rss"
  # buckets: caught by a window 2x / 4x / 16x as wide around the same centre, or beyond
  awk -v n="$1" '/^ovwhere/ {sub(/^ovwhere *: */,""); print "ovwhere " n " " $0}' <<< "$out"
  awk -v n="$1" '/^levelmax/ {sub(/^levelmax *: */,""); print "levelmax " n " " $0}' <<< "$out"
}

diag id8192   1 0 8192 16
diag fib8192  1 1 8192 16
diag fib4096  1 1 4096 16
diag fib1024  1 1 1024 16
diag fib512   1 1 512 16
diag gid8192  2 0 8192 16
diag gfib8192 2 1 8192 16
diag fib8192l8  1 1 8192 8
diag fib16384l8 1 1 16384 8
echo "# end $(date -Iseconds)"
