# itch-engine

A NASDAQ TotalView-ITCH 5.0 feed handler and L3 order book reconstruction engine, in C++23.

Written to find out how fast this can actually go on one core, and to have something concrete to point at when people ask what I've built. No runtime dependencies: Catch2 and Google Benchmark are test-only.

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
