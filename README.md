# itch-engine

A NASDAQ TotalView-ITCH 5.0 feed handler and L3 order book reconstruction engine, in C++23.

Written to find out how fast this can actually go on one core, and to have something concrete to point at when people ask what I've built. No runtime dependencies. The unit tests are plain-assert executables (Catch2 and Google Benchmark are vendored in `third_party/` but not wired up yet).

A write-up of the architecture, the measurements and the design decisions they drove is in [`paper/itch-engine.pdf`](paper/itch-engine.pdf). Raw benchmark logs are in [`results/`](results/).

## Layout of one message
||||
|---|---|---|
|**Before the message** | **The message itself** |
|2-byte length (big-endian)|11-byte HEADER|TYPE-SPECIFIC BODY
|tells you how far to jump| every message has this, always 11 bytes|different per type|

**Header, byte by byte:**

off 0: msg_type (1) 'A', 'D', 'U', ... what kind of message\
off 1: stock_locate (2) which instrument -> routing key\
off 3: tracking (2) (we ignore)\
off 5: timestamp (6) nanoseconds since midnight

Example, an ADD (type 'A', body = 25 bytes, total 36):

```text
[len=36][A|loc=2|trk|ts.......|order_ref|B|shares|SYMBOL..|price]
         \___________________/ \_________________________________/
               header (11 B)              body (25 B)
```

**Messages processor**
```text
file:  [len][ msg ][len][   msg   ][len][ msg ][len][  msg  ] ...
          |    |      |    |          |
          |    |      |    |          +-- read len, jump
          |    |      |    +-- decode, apply to book, jump by len
          |    |      +-- read 2-byte length
          |    +-- decode this message, apply to book
          +-- read 2-byte length -> "next msg is 36 bytes"
```

p starts at the file begin.\
loop:\
    len = read 2 bytes at p           "how big is the next one"\
    p   += 2\
    type = byte at p                  "what kind is it"\
    loc  = 2 bytes at p+1             "which instrument"\
    ... decode the fields we need, apply to books[loc] ...\
    p   += len                        "jump to the next message"

The jump (p += len) is what makes it a stream walk. You never search for message boundaries, the length told you exactly where the next one starts. Unknown message types (H, I, Q, the ones you skip) cost nothing: you still read their length and jump over them without looking inside.

Messages for all instruments are interleaved in time order:
```text
  time -->
  [A loc=2][A loc=5][D loc=2][A loc=1][X loc=5][U loc=2][D loc=1] ...
        |        |       |        |        |       |        |
        v        v       v        v        v       v        v
     book[2]  book[5] book[2]  book[1]  book[5] book[2]  book[1]
```

Each message carries its own stock_locate, so you demultiplex the single stream into thousands of per-instrument books on the fly. That's the whole job: one interleaved stream in, thousands of live books out, and the stock_locate on every message is what tells you which book each event belongs to.

```text
locate 2:  bid 287800 x500  |  ask 287900 x200   ok
             \_____________/     \_____________/
              highest price       lowest price
              someone will        someone will
              BUY at ($28.78)     SELL at ($28.79)

  the 1-cent gap between them is the SPREAD.
  "ok" = bid < ask, a sane market. crossed (bid >= ask) would mean a bug.
```
That's 64.9M messages replayed into live state with zero missing refs and no crossed books. The reconstruction is correct.

## Status

Current: **M3**, parser and book split across a lock-free single-producer/single-consumer queue, validated identical to the single-threaded path over all symbols. Measured properly, the split is slightly slower on cached data (see *M3 timing* below), which is itself the result: the book, not the parser, is the bottleneck.

How it got here:

**M1a, framing layer.** mmap the file, walk it by the 2-byte length prefix, classify every message by its type byte. No field decoding at all. This is the ceiling everything else gets measured against.

**M1b, field decode.** Packed structs overlaid on the mapped bytes via memcpy, a switch dispatching on the type byte, lazy big-endian conversion only on the fields actually consumed. Also collapsed the 48-bit timestamp decode from 11 operations to 3.

**M2, the book.** Full L3 reconstruction per symbol: unordered_map from order ref to order, two std::maps for the bid and ask price levels. Correct and slow, deliberately, because it became the oracle for everything after it.

**M2b, price ladder.** Price levels moved from a std::map to a flat array indexed by price, with a lazily allocated per-symbol window, an overflow map for out-of-window and sub-penny prices, and a memoized hint for finding the best level.

**M2c, open-addressed order table.** Replaced unordered_map for the by-ref lookup with a flat slot array, linear probing, backward-shift deletion.

**M3, the thread split.** A producer thread parses and decodes each message into a fixed-size Command; a consumer thread pops Commands and applies them to the book. They share nothing but a lock-free SPSC ring buffer. The book is touched by exactly one thread, so it needs no locks.

## Numbers

Parsed 64,858,229 messages from a 1.9 GiB slice of `01302019.NASDAQ_ITCH50`.

Each layer costs something. Warm, single-threaded, same slice:

| stage | M msg/s |
|---|---|
| framing only (count messages, touch 3 bytes) | 258 |
| + field decode (memcpy overlay, byteswaps) | 127 |
| + L3 book, std::map levels + unordered_map refs | 1.8 |
| + price ladder (flat array levels) | 2.3 |
| + open-addressed order table | 3.8 |

The book costs two orders of magnitude more than parsing. That's the whole story of M2: it isn't compute, it's memory.

The framing number on its own:

| | |
|---|---|
| Throughput (warm) | **258 M msg/s**, 8.0 GB/s |
| Throughput (cold) | 43 M msg/s, 1.3 GB/s |
| Heap allocations in the parse loop | 0 |
| Single-threaded | yes |

Count-only: the loop reads the 2-byte length prefix and the message type byte, then jumps. It touches 3 bytes out of an average 29-byte message. This is a memory-bandwidth number more than a compute number, and it's the ceiling that everything after it gets measured against, not a claim about a finished parser.

**Warm** means the file is in the page cache. The 6x cold/warm gap is the disk, not the code. Quoting the warm figure without saying so would be dishonest.

Measured on an AMD Ryzen 7 4700U, 7 GB RAM, under WSL2 (kernel 6.18.33.2-microsoft-standard-WSL2), GCC 13.4, `-O3 -march=native`. That is a 15W mobile part inside a hypervisor, so treat every wall-clock figure here as a floor with a wide error bar, not as a characterisation of the code on server hardware.

**M3 timing, revisited.** Initially deferred: WSL has no core pinning, so a threaded-vs-single number risked measuring the scheduler, not the design. 

Measured anyway, twice. The first attempt (10 runs each, trimmed mean) reported threaded ~5% faster, 3.97 vs 3.78 M msg/s. **That result is withdrawn**: a later check of the method found the two binaries weren't timed the same way and had been built from uncommitted code.

Remeasured on a clean clone of `5f9ee3c` (`results/bench_m3.sh`): both binaries timed by their own `steady_clock` around the work loop only, single and threaded runs **interleaved** so drift hits both equally, page cache warmed before every run. Ultimate Performance power plan, AC power, browser and sync clients closed.

| input | pairs | single (median) | threaded (median) | threaded wins |
|---|---|---|---|---|
| 2 GB slice, fits in page cache | 25 | 4.67 M msg/s (sd 0.29) | 4.52 M msg/s (sd 0.17) | 5 / 25 |
| full day, 11 GB, larger than WSL's 7 GB | 9 | 4.08 M msg/s (sd 0.27) | 4.35 M msg/s (sd 0.32) | 6 / 9 |

**When the data is in memory, the split is ~3% slower.** That's consistent with the cost ratio above (decode ~4 ns/msg, apply_command ~255 ns/msg, 64:1). Moving decode off the consumer saves almost nothing, and every message now pays a cross-core handoff of a 40-byte Command, plus the cost of two cores spinning.

**On the uncached full day it's ahead**, plausibly because the producer takes the page-fault and disk stalls while the consumer keeps applying, so I/O overlaps with work. That's a hypothesis, not a finding: 9 pairs with this much noise can't settle it, and the full-day runs are partly disk-bound by construction.

Neither result uses core pinning (no taskset, no thread affinity), so this is reduced noise, not a scheduler-proof measurement. The ceiling stays hard either way: apply_command is the bottleneck however many threads there are, so the next real gain is sharding apply across consumer threads by stock_locate, not tuning this pipeline.

## What the counters say

Wall clock on this box has a 20–25% run-to-run spread when anything else is going on, so it can't resolve a 20% change. I rank on perf counters instead.

`perf stat` on the 2 GB slice, user space only (`:u`), commit `2d8d929` (`results/counters_reps5.txt`). Each variant is built once and run 5 times, interleaved (every variant once, then every variant again), so drift spreads across all of them. Median, with min–max in brackets. "Paired" is how many of the 5 rounds the variant missed more than the per-book fibonacci W=8192 table in the same round, which cancels drift that moves a whole round. Probes per lookup come from a separate untimed build with `-DPROBE_STATS` (`results/diag.txt`).

| variant | instr (G) | miss/msg | paired | M msg/s | probes/lookup |
|---|---|---|---|---|---|
| unordered_map, W=8192 | 18.59 | 14.61 (14.24–15.37) | 5/5 | 2.4 (2.3–2.6) | – |
| per-book, identity, W=8192 | 13.49 | 8.82 (8.59–9.06) | 5/5 | 4.2 (4.1–4.7) | 2.44 |
| **per-book, fibonacci, W=8192** | **12.91** | **8.34 (8.24–8.69)** | – | **4.8 (4.5–5.1)** | **1.57** |
| per-book, fibonacci, W=4096 | 13.99 | 9.55 (9.54–9.57) | 5/5 | 4.6 (4.5–4.9) | 1.57 |
| per-book, fibonacci, W=1024 | 16.13 | 12.79 (12.54–12.83) | 5/5 | 3.9 (3.8–3.9) | 1.57 |
| per-book, fibonacci, W=512 | 16.87 | 13.85 (13.74–14.05) | 5/5 | 3.6 (3.4–3.7) | 1.57 |
| global, identity, W=8192 | 554.18 | 16.88 (16.46–17.34) | 5/5 | 1.1 (1.0–1.2) | 707.21 |
| global, fibonacci, W=8192 | 11.74 | 9.52 (9.47–9.99) | 5/5 | 4.7 (4.3–5.1) | 1.70 |

Instruction counts were identical in all 5 runs of every variant. With the machine left alone, misses stayed within about ±5% and throughput within about ±8%.

**The order table is a real win.** 31% fewer instructions, 43% fewer misses per message, 2x the throughput. Chained maps cost a bucket-array miss plus a pointer chase to a heap node; flat slots with linear probing cost one miss, and the probe walks forward through contiguous memory, which the prefetcher handles for free. Deletion is backward-shift rather than tombstones, because 41% of messages are deletes and tombstoned slots still have to be probed through, so chains would grow without bound.

**Ladder width matters, in the opposite direction from what I predicted.** Going from 8192 slots to 512 raises misses by 66% and instructions by 31%, worse with every halving. I expected a smaller ladder to fit in cache and win. The array part of that is true, but every price outside the window goes to the `std::map` overflow, and the overflow adds go from 4.0M at 8192 to 20.4M at 512. A tree walk through heap nodes costs far more than a bigger array saves.

So why not go wider? 4.0M adds still overflow at 8192, so I classified each one by the window that would have caught it (`ovwhere` in `results/diag.txt`): 48% would fit in 2x the width, 3% are sub-penny and never fit a ladder, and the rest are far from the market (22% need 4x, 14% need 16x, 12% not even that), 85% of them above the window. Meanwhile a W=8192 run already peaks at 4.2 GiB resident (ladders plus the mapped file) on a 7 GB WSL, and going from 4096 to 8192 alone added 1.1 GiB. Doubling the ladders' memory to catch about half the overflow is maybe 5–9% fewer misses, before the bigger footprint costs anything back. Not worth it, 8192 stays. The cheaper route is a smaller `Level`: it's 16 bytes now, and if share totals fit in 32 bits it could be 8, which buys 2x the width at today's memory.

**One global order table didn't pay off.** ITCH refs are unique for the day, so mode 2 keeps every order in one table keyed by ref, with a direct `Level*` per order: execute/cancel/delete skip the book object and find the order once (68.4M lookups instead of 105.9M). Same shape as [WK Selph's order book](https://gist.github.com/halfelf/db1ae032dc34278968f8bf31ee999a25) and [charles-cooper/itch-order-book](https://parallelprogrammer.substack.com/p/fun-new-project-itch-order-simd). It runs 9% fewer instructions but has 14% more misses per message, in every round, and throughput is no better. My read: one table holding every live order is big and cold, while a busy symbol's small table stays hot. That fits the counters but I haven't isolated it. Per-book tables stay the default; mode 2 stays in the code as the measured alternative.

**Hash choice: small per book, critical globally.** Per-book, fibonacci is 4% fewer instructions and 5% fewer misses, lower in all 5 rounds. Identity hashing puts near-sequential refs in neighbouring slots, and linear probing merges those into long clusters: 2.44 slots per lookup against 1.57, with runs up to 1,892. The per-book tables are small and cached, so that barely costs anything. In one global table the clusters never stop growing: 707 slots per lookup, longest probe 105,698, 43x the instructions. Fibonacci's value is robustness, not speed.

**Correction.** An earlier version of this README (and the paper) said ladder width does nothing. The `-DLADDER_WIDTH` flag was never read, the header hard-coded 8192, so that sweep compared four identical binaries and the spread was noise. Every binary now prints the config it was compiled with, and `bench_counters.sh` refuses to record a run that doesn't match the flags it passed. The old numbers also counted user+kernel, so they aren't comparable with the table above; they're kept in `results/counters.txt`.

## How I know it's right

Five independent checks, because "it didn't crash" isn't evidence.

**1. The message mix looks like a real trading day.**

```
A  43.20%   add order
D  40.80%   delete
U   7.80%   replace
X   2.66%   cancel
E   1.71%   execute
F   1.48%   add w/ MPID
```

If the length arithmetic drifted by a single byte anywhere in 65M messages, the walk would desynchronize and I'd be reading random bytes as type characters. The histogram would be noise. It isn't.

**2. Struct layouts match the wire.**

Every handled message type has a packed struct whose `sizeof()` must equal the on-wire payload length. A debug build asserts this on every message, 65M of them, zero failures. So there's no hidden padding and no wrong field widths, verified against real data rather than against my reading of the spec.

**3. Orders are conserved.**

Every order that rests in the book has to leave it eventually. In via `A`, `F`, `U`. Out via `D`, `X`, `E`, `U`. (`U` is a replace, it kills one order ref and creates another, so it sits on both sides.)

```
A+F+U     = 34,034,275
D+X+E+U   = 34,353,983
ratio     = 1.009
```

0.9% residual, which is orders still resting when my slice was cut, plus partial fills. Nothing in the code enforces this identity. The data agreeing with it is the real signal.

**4. Two independent books agree exactly.**

The naive book (std::map levels, unordered_map refs) and the fast one (flat ladder, open-addressed table) run side by side over the whole file and get compared on best bid and best ask for every symbol. 0 mismatches, 0 missing refs, no crossed books, 64.9M messages. That is why the slow one was written first: it's the oracle, and every optimisation after it gets checked against it before it gets measured.

Caveat: differential testing only catches divergence between the two books, not a bug both share. Found one: decode() and the naive path both read side off the wire and hand it to an unvalidated (side == 'B') ? bids : asks ternary, so a corrupt byte misroutes identically in both, invisible to the diff. Fixed by validating at each book's own entry point (itch::is_valid_side()) rather than trusting the A/B comparison to catch it. Regression test: tests/test_decode_side.cpp.

**5. The threaded book equals the single-threaded book.**

Both paths dump every active symbol's best bid and ask to a file at the end. Over about 8,000 active symbols the two files are byte-for-byte identical. A queue that dropped a message or reordered one would show up as a diff. None does, so the lock-free path preserved order and lost nothing.

## Design notes

* **mmap, not read().** `MAP_PRIVATE` + `MADV_SEQUENTIAL`, the whole file mapped as one contiguous region. No buffers, no copies, just pointer arithmetic.  
read() pays a mandatory copy_to_user regardless of buffer size (~670ms for 10GB at ~15GB/s memcpy bandwidth).  
mmap's cost is ~2.6M minor faults, ~4ms total. The kernel pages it in and drops pages behind us.  
MAP_PRIVATE not SHARED: the mapping is PROT_READ-only and never mutated, so copy-on-write never triggers, inert today, just documents intent in case PROT_WRITE is ever added.
* **The wire drives advancement, not `sizeof`.** The length prefix tells me how far to jump, so I skip message types I've never implemented without knowing anything about them.
* **memcpy overlay, not reinterpret_cast.** Nothing ever constructed an AddOrder at that address, so a pointer cast is strict-aliasing UB. memcpy into a local is the sanctioned way to say "read these bytes as that type", and at -O3 it compiles to nothing.
* **`std::array<uint64_t, 256>` for counts**, indexed by the raw type byte. No hashing, no allocation, one cache line. Indexed as `uint8_t`, not `char`, because `char` is signed on x86 and a negative array index is UB.
* **Big-endian conversion is lazy.** ITCH is big-endian; I `std::byteswap` per field, only for fields I actually consume. Prices stay `uint32_t` fixed-point with 4 implied decimals. No floats anywhere near the hot path.
* **The timestamp has two decoders.** The fast one loads 8 bytes, byteswaps once and shifts right, which over-reads the 6-byte field by 2. Fine for any message of 13 bytes or more. SystemEvent is only 12, so it keeps the byte-at-a-time version. `-Warray-bounds` caught that, not me.
* **One routing decision for prices.** `to_slot()` decides whether a price lives in the ladder or in the overflow map, and both the add path and the find path call it. If those two ever disagreed, the accounting would break silently.
* **SPSC queue, no locks.** Single producer, single consumer, so no compare-and-swap: each thread does a relaxed load of its own index, an acquire load of the other's, and a release store after touching the slot. head and tail sit on separate cache lines (`alignas(64)`) so the two cores don't ping-pong one line between them. Indices are monotonic and masked only on access, so head-tail==CAP means full and head==tail means empty with no wasted slot.
* **Command is a flat POD.** One fixed-size struct per book operation, trivially copyable so it memcpys into the ring. It carries everything a book call needs by value and points back into nothing, because by the time the consumer reads it the parser has moved far ahead in the file.
* **50% load factor before growing, not 70-90%.** `insert()` doubles capacity when occupancy would cross half. At 50%, expected probes are ~1.5 for a hit, ~2.5 for a miss (uniform hashing, open addressing); both climb non-linearly toward 80%. Hits dominate here, so the win isn't the average case, it's bounding tail probe length right before a `grow()`. Costs ~6 MB extra at 200k orders versus an 80% table; trivial next to L3.

### Decode safety: copy-then-decode

Every hot-path case in `main.cpp` copies the wire bytes into a stack-local
struct (`memcpy(&m, p, sizeof(m))`) before calling any accessor, rather than
`reinterpret_cast`-ing the mmap'd pointer `p` directly. This buys two things:

1. **Strict-aliasing correctness.** Reinterpreting raw bytes as a typed object
   without `memcpy` (or `std::start_lifetime_as` in C++23) is UB per the
   standard, regardless of `#pragma pack(1)` packing controls layout, not
   aliasing rules.
2. **mmap tail-of-file safety.** `load_be48` (messages.hpp) trades a cheap
   6-byte field decode for a single 8-byte load + bswap + shift it reads 2
   bytes it doesn't need. If run directly against the mmap pointer, that
   overread is safe for every message except the last one in the file: when
   the file size happens to be an exact multiple of the page size (4096B), the
   final message has zero trailing slack in its page, so the extra bytes cross
   into an unmapped page and the read is illegal.

Copying onto the stack first sidesteps both: the stack copy isn't the mmap'd
object (no aliasing issue), and it has whatever the program's live stack slack
is around it, orders of magnitude more room than the byte or two `load_be48`
ever over-reads. Cost: a ≤44-byte copy per message, negligible next to the
segfault it avoids.

## Build
On WSL

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-13
cmake --build build -j
./build/itch_engine          /path/to/01302019.NASDAQ_ITCH50    # single-threaded
./build/itch_engine_threaded /path/to/01302019.NASDAQ_ITCH50    # producer/consumer split
```

Build variants are compile-time flags: `BOOK_MODE` (0 naive, 1 ladder, 2 both plus A/B validation), `TABLE_MODE` (0 unordered_map, 1 open-addressed per book, 2 one global open-addressed table), `HASH_MODE` (0 identity, 1 fibonacci), `LADDER_WIDTH`, and `PROBE_STATS` (diagnostics only, never in a benchmarked build). Every binary prints a `config :` line with what it was actually compiled with.

```bash
REPS=5 ./results/bench_counters.sh
```

builds the whole matrix and runs perf on each variant. Every variant gets a **fresh** build directory, deliberately: incremental builds on a /mnt/c source tree silently skip recompiles when the Windows and WSL clocks disagree, and a stale binary once gave me a confident and completely wrong "the hash table bought nothing" reading. The build directories live on WSL's own disk (`~/itch-bench-builds`), because an IDE watching the repo kept deleted /mnt/c build dirs "delete pending" and broke CMake's configure.

To check the threaded path against the single-threaded one, both binaries take an optional second argument, a dump file of every active book's top of book:

```bash
./build/itch_engine          data.bin single.txt
./build/itch_engine_threaded data.bin threaded.txt
diff single.txt threaded.txt && echo IDENTICAL
```
Requires GCC 13+ or Clang 17+ (needs `std::byteswap`), CMake >= 3.25. Linux only, `mmap` is POSIX.

### Tests

Plain-assert executables, not registered with CTest yet (there's no `tests/CMakeLists.txt`). Build them standalone with asserts on:

```bash
g++-13 -std=c++23 -O2 -Isrc tests/test_spsc.cpp        -o test_spsc        && ./test_spsc
g++-13 -std=c++23 -O2 -Isrc tests/test_decode_side.cpp -o test_decode_side && ./test_decode_side
```

`test_spsc`: fill to capacity, refuse when full, FIFO order, wrap-around. `test_decode_side`: valid sides accepted, malformed sides rejected on `'A'`/`'F'`. Both pass on `5f9ee3c` and `2d8d929` (`results/tests.txt`).

### Benchmark scripts

* `results/bench_counters.sh`: the perf counter matrix. `REPS=N` builds once and interleaves N rounds, then prints medians and min–max. Events are explicitly `:u`, so results don't depend on `kernel.perf_event_paranoid` (WSL resets it to 2 on restart). Stops with `CONFIG MISMATCH` if a binary wasn't built with the flags it was asked for.
* `results/bench_diag.sh`: overflow adds per ladder width and probes per lookup per table, from `-DPROBE_STATS` builds. Untimed, it explains the counter matrix rather than replacing it.
* `results/bench_m3.sh <file> <pairs> <out>`: single vs threaded, both timed by their internal clock, runs interleaved, page cache warmed before each run.
* `results/make_slice.cpp`: recreates the 64,858,229-message slice (first 2 GB of the day, cut at a message boundary).

`bench2.sh` is kept for history but its comparison is flawed: it times single-threaded with the internal clock and threaded with process wall time. Use `bench_m3.sh`.

Sample data: `emi.nasdaq.com`. Not in the repo, it's 11 GB.

## Roadmap

- [x] **M1a**, mmap + framing walk, count-only
- [x] **M1b**, struct overlay, field decode, dispatch switch
- [x] **M2**, L3 order book reconstruction, validated
- [x] **M2b**, price ladder, A/B validated against the naive book
- [x] **M2c**, open-addressed order table, backward-shift deletion
- [X] **M3**, lock-free SPSC queue, parser and book on separate threads
- [ ] **M4**, MoldUDP64 transport, gap detection, recovery
- [ ] latency histograms (rdtsc), not just throughput
- [x] M3 throughput under a controlled environment, same-clock interleaved runs: threaded ~3% *slower* on cached data (4.52 vs 4.67 M msg/s), ahead on the uncached full day (4.35 vs 4.08, noisy). Supersedes an earlier, flawed +5% figure
- [ ] true pinned-core measurement (taskset/thread affinity), to rule out scheduler noise rather than just reduce it
- [ ] shard apply_command across multiple consumer threads by stock_locate; the actual next throughput lever, since apply is the bottleneck regardless of thread count

- [x] global order table with direct level pointers (mode 2): measured, not kept, see "What the counters say"
- [x] wider ladder window: classified the 4M overflow adds at W=8192, only half would fit in 2x the width at 2x the memory. Not worth it
- [ ] 8-byte `Level` (32-bit share total, if the data allows it): half the ladder memory and cache footprint

Open: about 8 cache misses per message remain in the book path. Next suspect is the books vector itself, 65,536 LadderBook objects indexed by an effectively random u16 on every message, which is a guaranteed cold miss on the book object before any work even starts. Mode 2 skips the book object on about half the messages but also changes the table size, so it can't separate the two effects.

## Run

Clean when I change CMake, compilers, or flags.
```bash
rm -rf build build-debug
```

Rebuild, after editing source.
```bash
cmake --build build -j
```

Warm the page cache.
```bash
cat ~/data/itch_2gb.bin > /dev/null
```