# itch-engine

A NASDAQ TotalView-ITCH 5.0 feed handler and L3 order book reconstruction engine, in C++23.

Written to find out how fast this can actually go on one core, and to have something concrete to point at when people ask what I've built. No runtime dependencies: Catch2 and Google Benchmark are test-only.

## Layout of one message
||||
|---|----|---|
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

Current: **M2c**, L3 order book with a price ladder and an open-addressed order table, A/B validated against a naive reference implementation.

How it got here:

**M1a, framing layer.** mmap the file, walk it by the 2-byte length prefix, classify every message by its type byte. No field decoding at all. This is the ceiling everything else gets measured against.

**M1b, field decode.** Packed structs overlaid on the mapped bytes via memcpy, a switch dispatching on the type byte, lazy big-endian conversion only on the fields actually consumed. Also collapsed the 48-bit timestamp decode from 11 operations to 3.

**M2, the book.** Full L3 reconstruction per symbol: unordered_map from order ref to order, two std::maps for the bid and ask price levels. Correct and slow, deliberately, because it became the oracle for everything after it.

**M2b, price ladder.** Price levels moved from a std::map to a flat array indexed by price, with a lazily allocated per-symbol window, an overflow map for out-of-window and sub-penny prices, and a memoized hint for finding the best level.

**M2c, open-addressed order table.** Replaced unordered_map for the by-ref lookup with a flat slot array, linear probing, backward-shift deletion.

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

## What the counters say

Wall clock on this box has a roughly 25% run-to-run spread, so it cannot resolve a 20% change. perf counters are deterministic, so those are what I rank on.

| variant | instr (G) | miss (G) | miss/msg |
|---|---|---|---|
| unordered_map, W=8192 | 21.42 | 1.08 | 16.73 |
| open-addressed, identity hash | 15.69 | 0.66 | 10.15 |
| open-addressed, fibonacci hash | 15.30 | 0.66 | 10.23 |
| fibonacci, W=4096 | 15.29 | 0.66 | 10.14 |
| fibonacci, W=1024 | 15.05 | 0.66 | 10.12 |
| fibonacci, W=512 | 15.04 | 0.65 | 9.97 |

Two results there, and one of them is a null.

**The order table is a real win.** 29% fewer instructions, 40% fewer cache misses. Chained maps cost a bucket-array miss plus a pointer chase to a heap node; flat slots with linear probing cost one miss, and the probe walks forward through contiguous memory, which the prefetcher handles for free. Deletion is backward-shift rather than tombstones, because 41% of messages are deletes and tombstoned slots still have to be probed through, so chains would grow without bound.

**Ladder width does nothing.** 8192 slots down to 512 is a 16x shrink in allocated memory, and miss/msg moves from 10.23 to 9.97. I expected a large win from fitting the ladder into cache and was wrong. The best-price hint means only a handful of slots near the touch are ever read, so the allocated size is irrelevant. What matters is the working set, not the array.

**Hash choice is a wash.** Identical misses, fibonacci about 2.5% fewer instructions. Took fibonacci, but it isn't a finding. The prediction that identity would win on locality (NASDAQ refs are near-sequential) failed because refs interleave across thousands of per-symbol tables, so consecutive refs never land in the same table anyway.

## How I know it's right

Four independent checks, because "it didn't crash" isn't evidence.

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

## Design notes

* **mmap, not read().** `MAP_PRIVATE` + `MADV_SEQUENTIAL`, the whole file mapped as one contiguous region. No buffers, no copies, just pointer arithmetic. The kernel pages it in and drops pages behind us.
* **The wire drives advancement, not `sizeof`.** The length prefix tells me how far to jump, so I skip message types I've never implemented without knowing anything about them.
* **memcpy overlay, not reinterpret_cast.** Nothing ever constructed an AddOrder at that address, so a pointer cast is strict-aliasing UB. memcpy into a local is the sanctioned way to say "read these bytes as that type", and at -O3 it compiles to nothing.
* **`std::array<uint64_t, 256>` for counts**, indexed by the raw type byte. No hashing, no allocation, one cache line. Indexed as `uint8_t`, not `char`, because `char` is signed on x86 and a negative array index is UB.
* **Big-endian conversion is lazy.** ITCH is big-endian; I `std::byteswap` per field, only for fields I actually consume. Prices stay `uint32_t` fixed-point with 4 implied decimals. No floats anywhere near the hot path.
* **The timestamp has two decoders.** The fast one loads 8 bytes, byteswaps once and shifts right, which over-reads the 6-byte field by 2. Fine for any message of 13 bytes or more. SystemEvent is only 12, so it keeps the byte-at-a-time version. `-Warray-bounds` caught that, not me.
* **One routing decision for prices.** `to_slot()` decides whether a price lives in the ladder or in the overflow map, and both the add path and the find path call it. If those two ever disagreed, the accounting would break silently.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-13
cmake --build build -j
./build/itch_engine /path/to/01302019.NASDAQ_ITCH50
```

Build variants are compile-time flags: `BOOK_MODE` (0 naive, 1 ladder, 2 both plus A/B validation), `TABLE_MODE` (0 unordered_map, 1 open-addressed), `HASH_MODE` (0 identity, 1 fibonacci), `LADDER_WIDTH`.

```bash
./bench.sh
```

builds the whole matrix and runs perf on each variant. Every variant gets a **fresh** build directory, deliberately: incremental builds on a /mnt/c source tree silently skip recompiles when the Windows and WSL clocks disagree, and a stale binary once gave me a confident and completely wrong "the hash table bought nothing" reading.

Requires GCC 13+ or Clang 17+ (needs `std::byteswap`), CMake >= 3.25. Linux only, `mmap` is POSIX.

Sample data: `emi.nasdaq.com`. Not in the repo, it's 11 GB.

## Roadmap

- [x] **M1a**, mmap + framing walk, count-only
- [x] **M1b**, struct overlay, field decode, dispatch switch
- [x] **M2**, L3 order book reconstruction, validated
- [x] **M2b**, price ladder, A/B validated against the naive book
- [x] **M2c**, open-addressed order table, backward-shift deletion
- [ ] **M3**, lock-free SPSC queue, parser/book on separate cores
- [ ] **M4**, MoldUDP64 transport, gap detection, recovery
- [ ] latency histograms (rdtsc), not just throughput

Open: about 10 cache misses per message remain. Next suspect is the books vector itself, 65,536 LadderBook objects indexed by an effectively random u16 on every message, which is a guaranteed cold miss on the book object before any work even starts.

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