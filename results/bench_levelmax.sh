#!/usr/bin/env bash
# Is a 32-bit share total per price level safe? Replays a whole day with -DPROBE_STATS and reports the largest
# share total and order count any level reached (ladder or overflow), plus a run with asserts on, where every add
# checks that the total can't wrap. Untimed.
#
# usage: ./results/bench_levelmax.sh <itch_file> | tee results/levelmax_fullday.txt

set -e
FILE=${1:?usage: $0 <itch_file>}
SRC=$(cd "$(dirname "$0")/.." && pwd)
COMMIT=$(git -C "$SRC" rev-parse --short HEAD)
git -C "$SRC" diff --quiet --ignore-cr-at-eol HEAD -- src CMakeLists.txt || COMMIT="$COMMIT+dirty"
echo "# start $(date -Iseconds) commit=$COMMIT file=$(basename "$FILE")"

build() {   # $1 = name, $2 = flags
  local d="$HOME/itch-bench-builds/$1"
  rm -rf "$d"
  cmake -S "$SRC" -B "$d" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-13 \
    -DITCH_BUILD_TESTS=OFF -DITCH_BUILD_BENCH=OFF -DCMAKE_CXX_FLAGS="$2" > /dev/null 2>&1
  cmake --build "$d" -j > /dev/null 2>&1
  echo "$d"
}

d=$(build levelmax-stats "-DPROBE_STATS -DBOOK_MODE=1 -DLEVEL_BYTES=16")   # 64-bit totals, so the true maximum is measured
"$d/itch_engine" "$FILE" | grep -E "^(config|messages|levelmax)"

d=$(build levelmax-assert "-UNDEBUG -DBOOK_MODE=1 -DLEVEL_BYTES=8")        # 32-bit totals, every add asserts no wrap
"$d/itch_engine" "$FILE" | grep -E "^(config|messages)"
echo "asserts: none fired (exit 0)"
echo "# end $(date -Iseconds)"
