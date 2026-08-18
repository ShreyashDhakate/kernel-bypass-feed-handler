# Baseline: memory hierarchy

Every latency target in this project is ultimately denominated in cache misses, so
the first measurement is what a miss costs on the development host. These numbers
are the reference the rest of the design is argued against.

## Host

| | |
|---|---|
| CPU | AMD Ryzen 7 5800H (Zen 3), 8 cores / 16 threads |
| L1d | 32 KiB per core, 64-byte lines |
| L2 | 512 KiB per core |
| L3 | 16 MiB shared |
| Memory | 7.6 GiB available to the guest |
| Kernel | Linux 6.18 (WSL2) |
| Compiler | GCC 15.2, `-O3 -march=native -std=c++20` |

Reproduce with:

```sh
make bench && ./build/bin/memory_hierarchy
./build/bin/memory_hierarchy --csv > docs/benchmarks/memory-hierarchy.csv
```

## Probe 1 — traversal order

A 4096 x 4096 `int32` matrix (64 MiB) summed twice: once along rows, once down
columns. Identical element count, identical additions, identical bytes touched.
Only the order differs.

| order | ns / element | relative |
|---|---:|---:|
| row-major | 0.30 | 1.0x |
| column-major | 11.21 | **37.3x** |

Row-major advances one element at a time, so a single 64-byte line fetch serves
16 consecutive `int32` accesses and the hardware prefetcher recognises the stream
well ahead of demand. Column-major advances one full row per step — 16 KiB — so
every access lands on a fresh line *and* a fresh 4 KiB page, adding a TLB miss to
each cache miss.

The measured 0.30 ns/element for the row-major case is below the cost of a single
load instruction, which is the expected signature of a vectorised, prefetched
stream: the loop is limited by memory bandwidth, not by access latency.

### Compiler interference

The first version of this probe reported a 0.9x ratio — the column-major loop
appeared *faster* than the row-major one. The cause was GCC's loop interchange
pass at `-O3`: recognising the column-major nest as inefficient, it swapped the
two loops and executed the row-major order in both cases, so the benchmark
measured the same code twice.

The fix is a memory-clobbering empty `asm` block at the end of each outer
iteration. It emits no instructions and blocks the interchange, while leaving the
inner loop free to vectorise. Worth recording, because it generalises: a
micro-benchmark whose result is discarded, or whose loop nest is rewritable, does
not measure what it appears to measure.

## Probe 2 — dependent-load latency

A randomly permuted Hamiltonian cycle over cache-line-sized nodes. Each load's
address is the previous load's result, so the prefetcher cannot run ahead and
memory-level parallelism is pinned at one outstanding miss. What is measured is
therefore the unloaded latency of one access at each working-set size.

| working set | ns / hop | resident in |
|---|---:|---|
| 8 KiB | 2.60 | L1d |
| 16 KiB | 2.59 | L1d |
| 32 KiB | 2.65 | L1d (at capacity) |
| 64 KiB | 6.82 | L2 |
| 128 KiB | 6.83 | L2 |
| 256 KiB | 7.01 | L2 |
| 512 KiB | 14.41 | L2 (at capacity) / L3 |
| 1 MiB | 24.12 | L3 |
| 2 MiB | 27.28 | L3 |
| 4 MiB | 29.21 | L3 |
| 8 MiB | 36.54 | L3 |
| 16 MiB | 115.06 | L3 (at capacity) / DRAM |
| 32 MiB | 140.89 | DRAM |
| 64 MiB | 158.31 | DRAM |
| 128 MiB | 171.56 | DRAM |

The steps land exactly where `lscpu` reports the capacity boundaries: flat through
32 KiB, a 2.6x step into the L2 plateau, a further step past 512 KiB, and the
collapse to DRAM once the set exceeds the 16 MiB L3. Nothing here was configured —
the curve is the cache hierarchy measuring itself.

## Consequences for the design

Taking the L1 hit as the unit of cost:

| event | cost | relative to L1 |
|---|---:|---:|
| L1 hit | 2.6 ns | 1x |
| L2 hit | 6.8 ns | 2.6x |
| L3 hit | 24–37 ns | 9–14x |
| DRAM access | 141–172 ns | 54–66x |

1. **One DRAM miss costs more than a hundred arithmetic operations.** For a decode
   path targeting sub-microsecond wire-to-strategy latency, the instruction count
   of the parser is close to irrelevant next to its memory access pattern.
2. **The hot working set must be kept inside L2 (512 KiB).** This bounds the
   descriptor ring, the packet buffer pool, and the order book's hot price levels.
   Sizing them is a capacity decision, not a convenience.
3. **Layout beats algorithm at this scale.** A 37x swing on identical arithmetic,
   from access order alone, sets the priority: choose the data layout first and
   the algorithm second.
4. **Copies are not free, they are line fetches.** This is the quantitative
   argument for the zero-copy decode path — a copy of a packet that will be read
   once pays full miss cost for no benefit.
