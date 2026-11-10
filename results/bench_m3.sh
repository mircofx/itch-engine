#!/usr/bin/env bash
# M3 single vs threaded, apples to apples.
# - Both modes timed by their own internal steady_clock (parse/apply only, excludes
#   process startup, mmap, book allocation, and the final dump). Wall clock also kept.
# - Runs are interleaved (S,T,S,T,...) so drift / thermal state hits both equally.
# - Page cache warmed before every run.
# usage: bench_m3.sh <file> <pairs> <outfile>
set -e
FILE=$1; N=$2; OUT=$3
B=build-m3
if [ ! -x $B/itch_engine ]; then
  rm -rf $B
  cmake -B $B -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-13 -DITCH_BUILD_TESTS=OFF -DITCH_BUILD_BENCH=OFF -DCMAKE_CXX_FLAGS="-DBOOK_MODE=1" >/dev/null
  cmake --build $B -j >/dev/null
fi
echo "# file=$FILE pairs=$N commit=$(git rev-parse --short HEAD) date=$(date -Is)" > $OUT
echo "# mode run internal_s msgs wall_s" >> $OUT
for i in $(seq 1 $N); do
  for mode in single threaded; do
    cat "$FILE" > /dev/null
    t0=$(date +%s.%N)
    if [ $mode = single ]; then
      o=$($B/itch_engine "$FILE" 2>/dev/null); s=$(awk "/^time/ {print \$3}" <<< "$o"); m=$(awk "/^messages/ {print \$3}" <<< "$o")
    else
      o=$($B/itch_engine_threaded "$FILE" 2>/dev/null); s=$(awk "/^threaded done in/ {print \$4}" <<< "$o"); m=-
    fi
    t1=$(date +%s.%N)
    echo "$mode $i $s $m $(echo "$t1 - $t0" | bc)" | tee -a $OUT
  done
done
