#!/usr/bin/env bash
# Benchmark matrix for itch-engine.
#
# Every variant gets a FRESH build directory. Incremental builds on /mnt/c are
# not trustworthy (Windows/WSL clock skew silently skips recompiles), and a
# stale binary produces a confident wrong conclusion, which is worse than a
# build error.
#
# Rank on miss/msg, not on M msg/s. The clock has a ~25% noise floor on this
# box; the hardware counters are deterministic.

set -e

DATA=~/data/itch_2gb.bin
MSGS=64858229          # message count in the slice, for the per-message column

build() {   # $1 = short name, $2 = compile flags -> echoes build dir
  local d="bench-$1"
  rm -rf "$d"
  cmake -B "$d" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-13 \
    -DITCH_BUILD_TESTS=OFF -DITCH_BUILD_BENCH=OFF \
    -DCMAKE_CXX_FLAGS="$2" > /dev/null
  cmake --build "$d" -j > /dev/null
  echo "$d"
}

run() {     # $1 = label, $2 = build dir
  cat "$DATA" > /dev/null          # warm the page cache before every run
  local out
  out=$(sudo perf stat -e instructions,cache-misses,cache-references \
        "$2/itch_engine" "$DATA" 2>&1)

  local rate ins mis ref
  rate=$(awk '/^rate/ {print $3; exit}' <<< "$out")
  ins=$(awk  '/instructions/     {gsub(/[^0-9]/,"",$1); print $1; exit}' <<< "$out")
  mis=$(awk  '/cache-misses/     {gsub(/[^0-9]/,"",$1); print $1; exit}' <<< "$out")
  ref=$(awk  '/cache-references/ {gsub(/[^0-9]/,"",$1); print $1; exit}' <<< "$out")

  # if a parse failed, show it rather than printing a silent zero
  : "${rate:=?}" "${ins:=0}" "${mis:=0}" "${ref:=0}"

  printf "%-22s %9s %11.2f %11.2f %11.2f %10.2f\n" \
    "$1" "$rate" \
    "$(bc -l <<< "$ins/1000000000")" \
    "$(bc -l <<< "$mis/1000000000")" \
    "$(bc -l <<< "$ref/1000000000")" \
    "$(bc -l <<< "$mis/$MSGS")"
}

printf "%-22s %9s %11s %11s %11s %10s\n" \
  VARIANT "M msg/s" "instr(G)" "miss(G)" "refs(G)" "miss/msg"

run "map,        W=8192" "$(build map8192  '-DBOOK_MODE=1 -DTABLE_MODE=0 -DLADDER_WIDTH=8192')"
run "table-id,   W=8192" "$(build id8192   '-DBOOK_MODE=1 -DTABLE_MODE=1 -DHASH_MODE=0 -DLADDER_WIDTH=8192')"
run "table-fib,  W=8192" "$(build fib8192  '-DBOOK_MODE=1 -DTABLE_MODE=1 -DHASH_MODE=1 -DLADDER_WIDTH=8192')"
run "table-fib,  W=4096" "$(build fib4096  '-DBOOK_MODE=1 -DTABLE_MODE=1 -DHASH_MODE=1 -DLADDER_WIDTH=4096')"
run "table-fib,  W=1024" "$(build fib1024  '-DBOOK_MODE=1 -DTABLE_MODE=1 -DHASH_MODE=1 -DLADDER_WIDTH=1024')"
run "table-fib,  W=512"  "$(build fib512   '-DBOOK_MODE=1 -DTABLE_MODE=1 -DHASH_MODE=1 -DLADDER_WIDTH=512')"