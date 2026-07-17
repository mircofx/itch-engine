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

Example, an ADD(type 'A', body = 25 bytes, total 36):\

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
    len = read 2 bytes at p "how big is the next one"\
    p   += 2\
    type = byte at p                  "what kind is it"\
    loc  = 2 bytes at p+1             "which instrument"\
    ... decode the fields we need, apply to books[loc] ...\
    p   += len                        "jump to the next message"\


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

**M1a, framing layer.** Done. The parser walks the raw ITCH file and classifies every message. It does not decode fields yet, that's next.

## Numbers

Parsed 64,858,229 messages from a 1.9 GiB slice of `01302019.NASDAQ_ITCH50`:

| | |
|---|---|
| Throughput (warm) | **258 M msg/s**, 8.0 GB/s |
| Throughput (cold) | 43 M msg/s, 1.3 GB/s |
| Heap allocations in the parse loop | 0 |
| Single-threaded | yes |

Count-only: the loop reads the 2-byte length prefix and the message type byte, then jumps. It touches 3 bytes out of an average 29-byte message. This is a memory-bandwidth number more than a compute number, and it's the ceiling that everything after it gets measured against, not a claim about a finished parser.

**Warm** means the file is in the page cache. The 6x cold/warm gap is the disk, not the code. Quoting the warm figure without saying so would be dishonest.

Measured on `<CPU>`, `<RAM>` GB, kernel `<uname -r>`, GCC 13.4, `-O3 -march=native`.

## How I know the framing is right

Three independent checks, because "it didn't crash" isn't evidence.

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

## Design notes

* **mmap, not read().** `MAP_PRIVATE` + `MADV_SEQUENTIAL`, the whole file mapped as one contiguous region. No buffers, no copies, just pointer arithmetic. The kernel pages it in and drops pages behind us.
* **The wire drives advancement, not `sizeof`.** The length prefix tells me how far to jump, so I skip message types I've never implemented without knowing anything about them.
* **`std::array<uint64_t, 256>` for counts**, indexed by the raw type byte. No hashing, no allocation, one cache line. Indexed as `uint8_t`, not `char`, because `char` is signed on x86 and a negative array index is UB.
* **Big-endian conversion is lazy.** ITCH is big-endian; I `std::byteswap` per field, only for fields I actually consume. Prices stay `uint32_t` fixed-point with 4 implied decimals. No floats anywhere near the hot path.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-13
cmake --build build -j
./build/itch_engine /path/to/01302019.NASDAQ_ITCH50
```

Requires GCC 13+ or Clang 17+ (needs `std::byteswap`), CMake >= 3.25. Linux only, `mmap` is POSIX.

Sample data: `emi.nasdaq.com`. Not in the repo, it's 11 GB.

## Roadmap

- [x] **M1a**, mmap + framing walk, count-only
- [ ] **M1b**, struct overlay, field decode, dispatch switch
- [ ] **M2**, L3 order book reconstruction, intrusive price levels
- [ ] **M3**, lock-free SPSC queue, parser/book on separate cores
- [ ] **M4**, MoldUDP64 transport, gap detection, recovery
- [ ] latency histograms (rdtsc), not just throughput


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