# itch-engine

A high-performance Nasdaq TotalView-ITCH 5.0 feed handler and limit order book in C.

> Status: M1 (parser) and M2 (order book) complete. M3 (performance) in progress.

## Getting the data

Nasdaq publishes free sample ITCH 5.0 files (search "Nasdaq ITCH sample files").
Each is one full trading day, gzipped and several GB. Decompress before use,
since the parser memory-maps the raw file:

```
gunzip -k 12302019.NASDAQ_ITCH50.gz
```

You will also want the official ITCH 5.0 specification PDF from Nasdaq.
It is the source of truth for every message layout.

## Build and run

```
make            # builds ./itch_stats
make test       # unit tests for the decoders
make asan       # rebuild with AddressSanitizer + UBSan

./itch_stats path/to/12302019.NASDAQ_ITCH50
./itch_stats --max-bytes 4G path/to/12302019.NASDAQ_ITCH50   # process only the first 4 GiB
```

`--max-bytes` accepts a plain byte count or a `K`/`M`/`G` suffix (binary units).
Processing stops cleanly at the last complete message before the limit.

## Layout

```
include/itch.h      message structs, big-endian readers, decoder API
src/decode.c        message length table + decoders
src/main.c          mmap + framing loop + stats (itch_stats)
tests/test_decode.c hand-built byte buffers checked against decoders
```

## Roadmap

- [x] **M1 Parser:** decode all order messages; zero framing errors on a full day
- [x] **M2 Order book:** per-stock book; custom hash table for order lookup
- [ ] **M3 Performance:** profile, then optimize memory access and lookups
- [ ] **M4 Networking:** replay over UDP multicast; detect gaps
- [ ] **M5 Python bindings:** expose the book to Python via cffi

## Design (M2)

| Component    | Structure | Why |
| ------------ | --------- | --- |
| Order table  | Open-addressing hash table: linear probing, backward-shift deletion, power-of-two capacity, Fibonacci hashing | Exec/cancel/delete messages carry only the order reference; one flat array, no per-order allocation |
| Books        | Array of 65,536 books indexed by stock locate | Locate is a 2-byte ID, so lookup is a direct index |
| Price levels | One sorted array per side, best price at the **end**; search from the end | Most activity is near the best price, so inserts/removals shift few elements and the best price is read in O(1) |

Each level stores price, total shares, and order count.

## Results: full trading day (30 Dec 2019)

**Correctness**: all consistency checks are 0 across 268,744,780 messages,
with identical results on macOS and Linux:

| Check | Count |
| ----- | ----- |
| Unknown order reference (exec/cancel/delete/replace for an order not in the table) | 0 |
| Unknown book (message for a stock with no directory entry) | 0 |
| Book errors (a level would go below zero, or shares/count disagree) | 0 |
| Duplicate order reference | 0 |
| Duplicate stock directory | 0 |
| Bad length / unknown type / bad side / zero shares / zero price / timestamps out of order or > 24h | 0 |

Every order added during the day was removed by the end of the day
(live orders at end: **0**).

**Data facts**

| Metric | Value |
| ------ | ----- |
| Stocks (directory entries) | 8,906 |
| Order references created (A + F + U) | 140,270,523 |
| Largest order reference | 308,631,072 |
| Peak live orders | 1,924,078 |
| Add-order price range | $0.0001 to $199,999.99 |
| Add orders priced ≥ $10,000 | 17,961 (~0.015%) |
| Add + Delete share of all messages | ~86% |

**Findings**

- **Far-away orders:** a small number of sell orders sit at deliberately unfillable prices (up to $199,999.99, the apparent maximum). The level arrays handle them at the far end, away from the hot path.
- **Order references are sparse:** only ~45% of the reference range (0 to 308.6M) is used. A direct array indexed by reference would need ~308M slots (~3.7 GB even at 12 bytes per slot), so the hash table is kept.
- **Table sizing:** starting at 2^20 slots, the table resized twice during the run (to 2^22). The peak of 1.92M orders fits in 2^22 slots at ~46% load.
- **Hot path:** Add Order (~44%) and Order Delete (~43%) dominate the feed, so nearly every message is a hash-table insert or delete.

## Benchmarks

**Environment:** Ubuntu VM (UTM on Apple M5, 7.7 GiB RAM), single thread.

**Workload:** the full day (8.25 GB) does not fit in the VM's page cache, so
benchmarks use the first 4 GiB of the file (`--max-bytes 4G`): 140.2M messages,
52% of the day. The slice includes the market open, has the same message mix as
the full day, and peaks at 1.78M live orders (vs 1.92M for the full day).

| Version | Data | Time | Throughput | Per message |
| ------- | ---- | ---- | ---------- | ----------- |
| M1: decode only   | first 2 GB slice | 0.40 s | ~175M msgs/s | ~6 ns |
| M2: decode + book | first 4 GiB (140.2M msgs) | 10.34 s ± 0.6% | ~13.6M msgs/s | ~74 ns |

**Method:** optimized build (`-O2`, no sanitizers), median of 5 runs via
`perf stat -r 5`, warm page cache verified (0 major page faults, ~1.0 CPUs utilized).

Adding the order book made processing ~12× slower per message. The order table
(4M slots × 24 bytes ≈ 100 MB) is much larger than the CPU caches, so most lookups
likely go to main memory (~100 ns each). Since ~86% of messages are a hash insert
or delete, this is consistent with the ~74 ns average. Milestone 3 will test this
with a profiler.

## M3: Performance

### Hybrid price-level search

**Why:** profiling (`perf record`, 4 GiB slice) showed `level_find`, the search for a
price level within one side of a book, at **32% of runtime**, almost as much as the
order-table lookup (38%). Levels are stored best-price-last and were searched linearly
from the end, on the assumption that activity concentrates near the best price.

**Change:** scan the best 8 levels linearly (the common case), then binary search the rest.
Both versions return the same position and found flag for every input
(differential test against the old linear search in `tests/test_stock_book.c`).

**Search statistics** (`make stats`, 150.3M searches; *depth* = levels from the best
price to the target, *work* = levels actually compared; percentiles are bucket upper bounds):

|                 | Depth | Work (hybrid) |
| --------------- | ----: | ------------: |
| Average steps   | 11.54 | **4.85**      |
| p50 steps ≤     | 3     | 3             |
| p90 steps ≤     | 31    | 15            |
| p99 steps ≤     | 255   | **31**        |
| p99.9 steps ≤   | 1023  | **31**        |

Before this change, work equalled depth: the linear search compared every level it passed.

| Steps     | Depth: searches |    % | cum % | Work: searches |    % | cum % |
| --------- | --------------: | ---: | ----: | -------------: | ---: | ----: |
| 0         |       9,010,880 |  6.0 |   6.0 |         90,927 |  0.1 |   0.1 |
| 1         |      59,079,921 | 39.3 |  45.3 |     65,976,805 | 43.9 |  43.9 |
| 2–3       |      26,866,213 | 17.9 |  63.2 |     28,010,181 | 18.6 |  62.6 |
| 4–7       |      20,444,028 | 13.6 |  76.8 |     20,824,502 | 13.9 |  76.4 |
| 8–15      |      17,501,264 | 11.6 |  88.4 |     27,737,764 | 18.5 |  94.9 |
| 16–31     |       8,213,194 |  5.5 |  93.9 |      7,687,794 |  5.1 | 100.0 |
| 32–63     |       5,046,371 |  3.4 |  97.2 |              0 |  0.0 | 100.0 |
| 64–127    |       2,187,032 |  1.5 |  98.7 |              0 |  0.0 | 100.0 |
| 128–255   |       1,051,689 |  0.7 |  99.4 |              0 |  0.0 | 100.0 |
| 256–511   |         511,125 |  0.3 |  99.7 |              0 |  0.0 | 100.0 |
| 512–1023  |         355,022 |  0.2 | 100.0 |              0 |  0.0 | 100.0 |
| 1024–2047 |          47,652 |  0.0 | 100.0 |              0 |  0.0 | 100.0 |
| 2048–4095 |          13,582 |  0.0 | 100.0 |              0 |  0.0 | 100.0 |

Each column counts searches independently: a search 1,000 levels deep falls in depth row
512–1023 but work row 16–31 (8 linear + ~11 binary-search comparisons). About 3% of searches
(64+ levels deep) accounted for roughly half of all levels walked by the old linear search.

Searches 8–15 levels deep now cost *more* comparisons than before (always 8 linear, then a
binary search over the whole remaining side), which is why the work 8–15 bucket is larger
than the depth one.

**Timing** (A/B in one session, alternating runs, `perf stat -r 5`, CPU time):

| Variant                     | task-clock       | vs. linear |
| --------------------------- | ---------------- | ---------: |
| Linear (original)           | 11.07 s ± 0.18%  | —          |
| Hybrid, 8-level probe       | 10.87 s ± 0.19%  | −1.9%      |
| Hybrid, 16-level probe      | 10.84 s ± 0.16%  | −2.1%      |
| Hybrid, 32-level probe      | 10.87 s ± 1.12%  | −1.8%      |
| Linear (repeat)             | 11.13 s ± 0.41%  | —          |
| Hybrid, 8-level probe (repeat) | 10.97 s ± 0.23% | −1.4%     |

Probe lengths 8–32 performed the same within noise; 8 was kept.

**Finding:** 58% fewer comparisons gave only ~2% more throughput. Each linear step was cheap
(~0.2 ns: sequential reads that the hardware prefetcher handles well, with predictable
branches). The remaining ~20 ns per search goes to waiting for the level array itself to arrive
from main memory, which points to prefetching as the next optimization. The hybrid also caps
worst-case search work at ≤31 comparisons instead of thousands, which matters for tail latency
even though average throughput barely moves.
