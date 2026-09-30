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

- [ ] **M1 Parser:** decode all order messages; zero framing errors on a full day
- [ ] **M2 Order book:** per-stock book; custom hash map for order_ref lookup
- [ ] **M3 Performance:** memory pools, cache-aware layout, perf-guided tuning
- [ ] **M4 Networking:** replay over UDP multicast; detect gaps
- [ ] **M5 Python bindings:** expose the book to Python via cffi

## Benchmarks

| Machine | Data | Condition | Time (median) | Throughput |
|---|---|---|---|---|
| Mac | Full day (8.25 GB, 268.7M msgs) | Warm cache | 2.18 s | ~123M msgs/s (~3.8 GB/s) |
| Ubuntu VM | Full day (8.25 GB, 268.7M msgs) | Can't cache: file ≈ all VM RAM | 7.66 s | ~35M msgs/s (limited by disk) |
| Ubuntu VM | 2 GB slice (69.4M msgs) | Warm cache | 0.40 s | ~175M msgs/s (~5.4 GB/s) |

Method: CLOCK_MONOTONIC around the main loop, file already in page cache
(run once to warm up, report the median of 5 runs).
