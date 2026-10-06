# itch-engine

A high-performance Nasdaq TotalView-ITCH 5.0 feed handler and limit order book in C.

> Status: Milestone 1 (parser) in progress.

## Getting the data

Nasdaq publishes free sample ITCH 5.0 files (search "Nasdaq ITCH sample files").
Each is one full trading day, gzipped and several GB. Decompress before use,
since the parser memory-maps the raw file:

    gunzip -k 01302019.NASDAQ_ITCH50.gz

You will also want the official ITCH 5.0 specification PDF from Nasdaq.
It is the source of truth for every message layout.

## Build and run

    make            # builds ./itch_stats
    make test       # unit tests for the decoders
    make asan       # rebuild with AddressSanitizer + UBSan

    ./itch_stats path/to/01302019.NASDAQ_ITCH50

## Layout

    include/itch.h      message structs, big-endian readers, decoder API
    src/decode.c        message length table + decoders
    src/main.c          mmap + framing loop + stats (itch_stats)
    tests/test_decode.c hand-built byte buffers checked against decoders

## Roadmap

- [x] **M1 Parser:** decode all order messages; zero framing errors on a full day
- [x] **M2 Order book:** per-stock book; custom hash table for order lookup
- [ ] **M3 Performance:** profile, then optimize memory access and lookups
- [ ] **M4 Networking:** replay over UDP multicast; detect gaps
- [ ] **M5 Python bindings:** expose the book to Python via cffi

## Design (M2)

| Component | Structure | Why |
|---|---|---|
| Order table | Open-addressing hash table: linear probing, backward-shift deletion, power-of-two capacity, Fibonacci hashing | Exec/cancel/delete messages carry only the order reference; one flat array, no per-order allocation |
| Books | Array of 65,536 books indexed by stock locate | Locate is a 2-byte ID, so lookup is a direct index |
| Price levels | One sorted array per side, best price at the **end**; search from the end | Most activity is near the best price, so inserts/removals shift few elements and the best price is read in O(1) |

Each level stores price, total shares, and order count.

## Results: full trading day (30 Dec 2019)

**Correctness**: all consistency checks are 0 across 268,744,780 messages:

| Check | Count |
|---|---|
| Unknown order reference (exec/cancel/delete/replace for an order not in the table) | 0 |
| Unknown book (message for a stock with no directory entry) | 0 |
| Book errors (a level would go below zero, or shares/count disagree) | 0 |
| Duplicate order reference | 0 |
| Duplicate stock directory | 0 |
| Bad length / unknown type / bad side / timestamps out of order | 0 |

Every order added during the day was removed by the end of the day
(live orders at end: **0**).

**Data facts**

| Metric | Value |
|---|---|
| Stocks (directory entries) | 8,906 |
| Order references created (A + F + U) | 140,270,523 |
| Largest order reference | 308,631,072 |
| Peak live orders | 1,924,078 |
| Add-order price range | $0.0001 to $199,999.99 |
| Add orders priced ≥ $10,000 | 17,961 (~0.015%) |

**Findings**

- **Far-away orders:** a small number of sell orders sit at deliberately unfillable prices (up to $199,999.99, the apparent maximum). The level arrays handle them at the far end, away from the hot path.
- **Order references are sparse:** only ~45% of the reference range (0 to 308.6M) is used. A direct array indexed by reference would need ~308M slots (~3.7 GB even at 12 bytes per slot), so the hash table is kept.
- **Table sizing:** starting at 2^20 slots, the table resized twice during the run (to 2^22). The peak of 1.92M orders fits in 2^22 slots at ~46% load.

## Benchmarks

| Version | Machine | Data | Time | Throughput | Per message |
|---|---|---|---|---|---|
| M1: decode only | Apple M5 (macOS) | full day | 2.18 s | ~123M msgs/s | ~8 ns |
| M1: decode only | Ubuntu VM (UTM) | 2 GB slice | 0.40 s | ~175M msgs/s | ~6 ns |
| M2: decode + book | Apple M5 (macOS) | full day | 18.03 s | ~14.9M msgs/s | ~67 ns |

Method: optimized build (`-O2`, no sanitizers), warm page cache, first run discarded, median of 5 runs, single thread.

Adding the order book made processing ~10× slower per message. The order table (4M slots × 24 bytes ≈ 100 MB) is much larger than the CPU caches, so most lookups go to main memory (~100 ns each), which matches the ~81 ns per message. Milestone 3 will test this with a profiler.
