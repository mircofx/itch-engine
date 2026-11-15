#!/usr/bin/env bash
# Benchmark matrix for itch-engine.
#
# Every variant gets a FRESH build directory. Incremental builds on /mnt/c are
# not trustworthy (Windows/WSL clock skew silently skips recompiles), and a
# stale binary produces a confident wrong conclusion, which is worse than a
# build error.
#
# Rank on miss/msg, not on M msg/s. The clock has a ~20-25% noise floor on this
# box. Instruction counts are deterministic; cache misses vary by up to ~10%
# between runs, so use REPS>=5 and compare medians.
#
# Events are counted in user space only (:u). That measures our code and not
# the kernel's page-fault work on the mmap, which is the same for every variant,
# and it does not depend on kernel.perf_event_paranoid. The 2026-11-10 run
# (counters.txt) had paranoid=0 and so counted user+kernel; WSL resets it to 2
# on restart, which silently made later runs user-only. Explicit :u removes
# that ambiguity.

set -e

DATA=~/data/itch_2gb.bin
SRC=$(cd "$(dirname "$0")/.." && pwd)   # repo root, so the script works from any cwd
MSGS=64858229          # message count in the slice, for the per-message column

build() {   # $1 = short name, $2 = compile flags -> echoes build dir
  # Build dirs live on WSL's native filesystem, not under /mnt/c: when a Windows process (e.g. an IDE's
  # folder watcher) holds a handle on a deleted build dir, Windows keeps it "delete pending" and
  # recreating it under the same name fails CMake's configure with "No such file or directory".
  local d="$HOME/itch-bench-builds/$1" attempt
  mkdir -p "$HOME/itch-bench-builds"
  for attempt in 1 2 3; do
    rm -rf "$d"
    if cmake -S "$SRC" -B "$d" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-13 \
         -DITCH_BUILD_TESTS=OFF -DITCH_BUILD_BENCH=OFF \
         -DCMAKE_CXX_FLAGS="$2" > /dev/null 2>&1 \
       && cmake --build "$d" -j > /dev/null 2>&1; then
      echo "$d"
      return 0
    fi
    echo "build '$1' failed (attempt $attempt), retrying" >&2
  done
  echo "build '$1' failed 3 times, giving up" >&2
  return 1
}

run() {     # $1 = label, $2 = build dir, $3 = expected config line
  cat "$DATA" > /dev/null          # warm the page cache before every run
  local out
  out=$(perf stat -e instructions:u,cache-misses:u,cache-references:u \
        "$2/itch_engine" "$DATA" 2>&1)

  # The binary prints what it was compiled with. If that isn't what we asked for, a -D was ignored
  # (this is how the original width sweep ended up comparing four identical binaries). Refuse to record it.
  local got
  got=$(awk '/^config/ {sub(/^config *: */,""); print; exit}' <<< "$out")
  if [ "$got" != "$3" ]; then
    echo "CONFIG MISMATCH for '$1': asked for [$3], binary reports [$got]" >&2
    exit 1
  fi

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

# TABLE_MODE: 0 = unordered_map per book, 1 = open-addressed per book, 2 = one global open-addressed table with order -> Level* (ladder_engine.hpp)
# HASH_MODE is ignored by TABLE_MODE 0 but passed anyway so every line of the config check is explicit.
#            label                 build name  TABLE HASH WIDTH
VARIANTS=(
  "map,        W=8192|map8192|0|0|8192"
  "table-id,   W=8192|id8192|1|0|8192"
  "table-fib,  W=8192|fib8192|1|1|8192"
  "table-fib,  W=4096|fib4096|1|1|4096"
  "table-fib,  W=1024|fib1024|1|1|1024"
  "table-fib,  W=512|fib512|1|1|512"
  "global-id,  W=8192|gid8192|2|0|8192"
  "global-fib, W=8192|gfib8192|2|1|8192"
)

REPS=${REPS:-1}   # e.g. REPS=5 ./results/bench_counters.sh
RAW=$(mktemp)
COMMIT=$(git -C "$SRC" rev-parse --short HEAD)
git -C "$SRC" diff --quiet HEAD -- src CMakeLists.txt || COMMIT="$COMMIT+dirty"
echo "# start $(date -Iseconds) commit=$COMMIT events=user-only reps=$REPS"

# Build everything first, then interleave the runs (rep 1 of every variant, then rep 2, ...),
# so slow drift in machine state spreads across all variants instead of biasing the last ones.
declare -A DIR
for v in "${VARIANTS[@]}"; do
  IFS='|' read -r label name t h w <<< "$v"
  DIR[$name]=$(build "$name" "-DBOOK_MODE=1 -DTABLE_MODE=$t -DHASH_MODE=$h -DLADDER_WIDTH=$w")   # set -e aborts on failure
done

printf "%-4s %-22s %9s %11s %11s %11s %10s\n" \
  REP VARIANT "M msg/s" "instr(G)" "miss(G)" "refs(G)" "miss/msg"

for rep in $(seq 1 "$REPS"); do
  for v in "${VARIANTS[@]}"; do
    IFS='|' read -r label name t h w <<< "$v"
    row=$(run "$label" "${DIR[$name]}" "BOOK_MODE=1 TABLE_MODE=$t HASH_MODE=$h LADDER_WIDTH=$w")   # set -e aborts on CONFIG MISMATCH
    printf "%-4s %s\n" "$rep" "$row"
    echo "$name $row" >> "$RAW"
  done
done

if [ "$REPS" -gt 1 ]; then
  # RAW columns: name, label (2 words), rate, instr, miss, refs, miss/msg
  med()   { sort -g | awk '{a[NR]=$1} END {if (NR%2) print a[(NR+1)/2]; else printf "%.2f\n", (a[NR/2]+a[NR/2+1])/2}'; }
  range() { sort -g | awk 'NR==1 {lo=$1} {hi=$1} END {printf "%s-%s\n", lo, hi}'; }
  col()   { awk -v n="$1" -v c="$2" '$1==n {print $c}' "$RAW"; }

  echo
  echo "MEDIAN of $REPS interleaved runs (min-max in brackets)"
  printf "%-22s %9s %13s %11s %10s %13s\n" VARIANT "M msg/s" "(range)" "instr(G)" "miss/msg" "(range)"
  for v in "${VARIANTS[@]}"; do
    IFS='|' read -r label name t h w <<< "$v"
    printf "%-22s %9s %13s %11s %10s %13s\n" "$label" \
      "$(col "$name" 4 | med)" "($(col "$name" 4 | range))" \
      "$(col "$name" 5 | med)" \
      "$(col "$name" 8 | med)" "($(col "$name" 8 | range))"
  done
fi
rm -f "$RAW"
echo "# end $(date -Iseconds)"
