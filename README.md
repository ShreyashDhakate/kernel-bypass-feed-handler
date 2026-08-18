# Ultra-Low-Latency Kernel-Bypass Feed Handler

A kernel-bypass market data pipeline in C++20: an emulated PCIe NIC, a poll-mode
user-space driver, a zero-copy ITCH 5.0 decoder, and a price-level order book —
with every latency claim measured rather than asserted.

The receive path performs no allocation, no copy, and no syscall.

---

## Table of contents

- [What this is](#what-this-is)
- [Why latency is worth money](#why-latency-is-worth-money)
- [Where the microseconds actually go](#where-the-microseconds-actually-go)
- [What kernel bypass changes](#what-kernel-bypass-changes)
- [The emulation trade](#the-emulation-trade)
- [Architecture](#architecture)
- [What each component demonstrates](#what-each-component-demonstrates)
- [Engineering rules this project holds itself to](#engineering-rules-this-project-holds-itself-to)
- [What this is not](#what-this-is-not)
- [Status](#status)
- [Results so far](#results-so-far)
- [Build and run](#build-and-run)
- [Layout](#layout)

---

## What this is

A **feed handler** is the first piece of software in an electronic trading system.
An exchange broadcasts every order, cancel, and trade on its book as a continuous
stream of UDP multicast packets. The feed handler's job is to receive those
packets, decode them, and maintain an in-memory replica of the exchange's order
book — so that a strategy always knows the current state of the market.

It is the most latency-critical component in the stack, because **every other
decision waits on it**. A strategy cannot price a quote it has not received. Any
delay here is added to the delay of everything downstream.

This project builds that path end to end, at the level real systems are built:

- a device that delivers packets into memory without the operating system involved,
- a driver that manages the device's descriptor rings from user space,
- a decoder that reads the exchange's binary protocol in place, without copying,
- an order book that maintains price levels under a continuous update stream,
- and instrumentation that reports what all of it actually cost.

Nothing is claimed here that a benchmark in `bench/` does not produce.

## Why latency is worth money

Exchanges match orders by **price-time priority**: at a given price, the order that
arrived first is filled first. That single rule turns latency into a direct
economic quantity.

**Queue position.** When a new price level opens, the orders that reach the
matching engine first sit at the front of the queue and get filled. The ones behind
them may never fill at all. Being consistently faster than competitors means being
consistently earlier in the queue.

**Adverse selection.** A market maker quotes a two-sided price. When the market
moves, those quotes become stale — they now offer a price better than fair value.
Faster participants will trade against them before they can be pulled. Every
microsecond of delay between "the market moved" and "my quote is cancelled" is
time spent exposed to being picked off. This is the dominant cost for a market
maker, and it is paid in *latency*, not in fees.

**Signal decay.** A short-horizon prediction — order book imbalance, a lead-lag
relationship between correlated instruments — is only profitable while it is still
true. Many such signals decay over microseconds to milliseconds. Acting on one late
is the same as not having it.

The competitive consequence: firms operating at this level measure **tick-to-trade**
latency — wire arrival to order departure — and treat it as a primary engineering
metric. Software stacks land in the low single-digit microseconds; FPGA
implementations reach into hundreds of nanoseconds. In that regime, a single
avoidable DRAM cache miss (**~170 ns on the host this was measured on**) is a
meaningful fraction of the entire budget. That is why this project starts by
measuring the memory hierarchy rather than by writing a parser.

## Where the microseconds actually go

The instinctive assumption is that a slow feed handler is slow because parsing is
slow. It is not. On a conventional Linux socket path, a UDP datagram traverses:

1. NIC DMA into a kernel ring buffer
2. an interrupt, then softirq processing
3. the network stack — IP, then UDP
4. a copy into the socket receive buffer
5. the application blocking in `recvmsg`, then being scheduled back in
6. a copy from kernel memory into the application's buffer
7. a return from the syscall, crossing the privilege boundary

Steps 2–7 are pure overhead from the application's point of view. They cost
several microseconds, and — worse for a trading system — they are **variable**.
Interrupt coalescing, scheduler decisions, and contention with other processes make
the tail far heavier than the median.

Tail latency is what matters here. A system whose median is 2 µs but whose 99.9th
percentile is 200 µs is not a 2 µs system; the slow path is exactly the path that
runs during a volatility burst, which is precisely when being late is most
expensive. This is why results in this repository are reported as distributions
with tail percentiles, never as a single average.

## What kernel bypass changes

Kernel bypass removes the operating system from the data path entirely. The
application maps the NIC's registers and DMA buffers into its own address space and
talks to the hardware directly:

- **No syscall.** Receiving is a load from a memory location the device writes.
- **No interrupt.** The driver polls a descriptor ring in a busy loop. Nothing
  wakes anything up; the thread is already spinning, pinned to a dedicated core.
- **No copy.** The device deposits the frame into a buffer the application already
  owns, and the decoder reads it there.
- **No context switch.** The privilege boundary is never crossed on the hot path.

The cost is real and worth stating plainly: a polling core is consumed at 100%
whether or not traffic arrives, memory must be pinned, and the application takes on
responsibility for things the kernel previously handled correctly on its behalf —
buffer lifetime, descriptor ownership, and memory ordering between two agents that
observe the same memory concurrently.

**That last responsibility is the actual engineering content of this project**, and
it is identical whether the device is real hardware or an emulator.

## The emulation trade

Production kernel bypass requires a supported NIC (Intel X710, Mellanox ConnectX,
Solarflare) bound to `vfio-pci`, an IOMMU, and hugepages. None of that exists on a
laptop under WSL2.

So the device is emulated. `nicsim` is a user-space process that behaves like a
PCIe network device: it exposes a register file with the semantics of a BAR,
consumes descriptors from rings in shared memory, writes payloads into buffers, and
signals completion through a doorbell and a completion counter rather than an
interrupt.

**Not reproduced:** PCIe transaction ordering, real DMA engines, IOMMU address
translation, and true MMIO write latency. The absolute numbers this produces are
not hardware numbers, and are never presented as such.

**Faithfully reproduced:** every contract the driver above the device must honour —
descriptor ring ownership, producer/consumer index discipline, memory ordering
between two concurrent agents, doorbell batching, zero-copy buffer lifetime, and
poll-mode operation. The driver is written against the same interface a real device
offers, so the device layer stays swappable: replacing `nicsim` with a VFIO-backed
device should not change the ring logic above it.

A conventional `AF_PACKET`/`AF_XDP` receive path is kept alongside as a **control**,
so bypass numbers are always quoted against a real kernel path measured on the same
host, in the same run.

## Architecture

```
   ┌──────────────────────────────────────────────────────────────────┐
   │  nicsim              emulated PCIe device                        │
   │  register file (BAR)  ·  RX/TX descriptor rings  ·  doorbells    │
   └───────────────────────────────┬──────────────────────────────────┘
                                   │  shared memory, no syscall
   ┌───────────────────────────────▼──────────────────────────────────┐
   │  driver              poll-mode user-space driver                 │
   │  ring ownership  ·  buffer pool  ·  acquire/release ordering     │
   └───────────────────────────────┬──────────────────────────────────┘
                                   │  pointer to received frame
   ┌───────────────────────────────▼──────────────────────────────────┐
   │  decode              MoldUDP64 framing → ITCH 5.0 messages       │
   │  in-place, big-endian, no allocation on the hot path             │
   └───────────────────────────────┬──────────────────────────────────┘
                                   │  lock-free SPSC ring
   ┌───────────────────────────────▼──────────────────────────────────┐
   │  book                price-level order book                      │
   └───────────────────────────────┬──────────────────────────────────┘
                                   │
   ┌───────────────────────────────▼──────────────────────────────────┐
   │  instrumentation     TSC timestamps · latency histograms         │
   └──────────────────────────────────────────────────────────────────┘
```

Design rationale in [docs/architecture.md](docs/architecture.md).

## What each component demonstrates

Each piece exists because it forces a specific competence that low-latency work
depends on.

| Component | Concept it forces | Why the discipline cares |
|---|---|---|
| `host_probe` | ABI, alignment, byte order, struct padding | Zero-copy decoding is only sound if the host's layout rules are known and asserted, not assumed |
| `memory_hierarchy` | Cache latency, spatial locality, prefetching, benchmark methodology | The unit of cost in this domain is the cache miss; also proves a benchmark measures what it claims |
| `nicsim` register file | MMIO, `volatile`, device vs memory semantics | A device register is an address that is not memory; loads and stores to it have side effects and cannot be cached, reordered, or elided |
| Descriptor rings | Producer/consumer ownership, wraparound, batching | The universal interface between a NIC and a driver; identical in DPDK, `io_uring`, and every vendor's datasheet |
| Poll-mode driver | Busy-wait vs interrupt, core pinning, syscall cost | Removing the kernel from the data path is the defining move of the field |
| DMA buffer pool | Pre-allocation, buffer lifetime, pinned memory | Allocation on a hot path is unbounded latency; ownership bugs here are silent corruption, not crashes |
| SPSC ring | Lock-free handoff, `acquire`/`release`, false sharing | Thread handoff without a mutex; and the measurable cost of two cores writing one cache line |
| ITCH 5.0 decoder | Binary protocol decode, big-endian, packed layout, branch behaviour | Real exchange protocol; in-place decode with no allocation is the production technique |
| Order book | Data structure choice under a hot update stream | The state every strategy reads; layout dominates asymptotic complexity at this scale |
| TSC instrumentation | `rdtsc`, invariant TSC, histograms, tail percentiles | Sub-microsecond timing needs a cycle counter; and an average latency hides the failure mode |

The through-line: **mechanical sympathy.** Understanding what the hardware and the
operating system are actually doing, and writing code that cooperates with them
rather than fighting them.

## Engineering rules this project holds itself to

These are consequences of the measurements in
[docs/benchmarks/memory-hierarchy.md](docs/benchmarks/memory-hierarchy.md), where a
DRAM access costs 54–66× an L1 hit and traversal order alone moved identical work
by 37×.

- **No allocation on the hot path.** Every buffer comes from a pool sized at
  startup. `new`, `malloc`, and any container that might grow are confined to
  initialisation. Allocation is unbounded latency, and the tail is what matters.
- **No syscalls on the hot path.** Receiving is a poll of a memory location.
- **Zero copy.** A frame is decoded in the buffer the device wrote it into.
- **Hot working set bounded by L2 (512 KiB).** Ring sizes, pool extent, and the
  book's resident price levels are chosen so steady-state footprint stays resident.
- **No shared cache lines between threads.** Producer and consumer indices sit on
  separate lines; anything crossing a thread boundary is padded, and its memory
  ordering is stated explicitly rather than left to a default.
- **Every claim is measured.** Reported as a distribution with tail percentiles,
  against the kernel path as a control, on a named host, reproducible from `bench/`.

## What this is not

Stated plainly, because inflated claims are worse than modest ones:

- **Not a production trading system.** There is no order entry, no risk layer, no
  exchange session management, no failover, and no gap recovery.
- **Not real hardware latency.** The device is emulated; absolute numbers are not
  NIC numbers. What transfers is the software contract above the device.
- **Not a DPDK replacement.** DPDK is a mature ecosystem with real PMDs. This
  builds the same concepts from scratch to understand them, not to compete.
- **Not FPGA-class.** The fastest tick-to-trade paths in the industry are in
  hardware. This is a software stack, with software's floor.

What it *is*: a correct, measured, from-scratch implementation of the receive path
that every one of those systems is built on top of.

## Status

Under active development. Components land with a benchmark and a document; the
table reflects what is actually merged.

| Component | Status |
|---|---|
| Host ABI verification (`host_probe`) | ✅ merged |
| Memory hierarchy baseline | ✅ merged |
| `nicsim` register file and BAR semantics | 🔜 next |
| RX/TX descriptor rings + doorbells | 🔜 |
| Poll-mode user-space driver | 🔜 |
| Lock-free SPSC handoff | 🔜 |
| MoldUDP64 + ITCH 5.0 zero-copy decoder | 🔜 |
| Price-level order book | 🔜 |
| TSC instrumentation + latency histograms | 🔜 |
| Kernel-path control (`AF_PACKET`/`AF_XDP`) | 🔜 |

## Results so far

Measured on AMD Ryzen 7 5800H (Zen 3), GCC 15.2 `-O3 -march=native`, Linux 6.18.
Full method and analysis: [docs/benchmarks/memory-hierarchy.md](docs/benchmarks/memory-hierarchy.md).

**Cost of a memory access, by where the data lives.** Dependent-load pointer chase
over a random cycle of cache-line-sized nodes, so the prefetcher cannot run ahead
and one miss is outstanding at a time:

| working set | resident in | ns / access | vs L1 |
|---|---|---:|---:|
| 32 KiB | L1d | 2.65 | 1.0× |
| 256 KiB | L2 | 7.01 | 2.6× |
| 4 MiB | L3 | 29.21 | 11× |
| 128 MiB | DRAM | 171.56 | **65×** |

The steps fall exactly on this CPU's documented cache capacities — the curve is the
hierarchy measuring itself, with nothing hardcoded.

**Cost of access order alone.** A 64 MiB matrix summed row-major and column-major:
identical arithmetic, identical bytes, only the order differs.

| order | ns / element |
|---|---:|
| row-major | 0.30 |
| column-major | 11.21 (**37.3×**) |

Together these set the project's priorities. A DRAM miss costs more than a hundred
arithmetic operations, so the decode path's memory layout matters far more than its
instruction count — and the hot working set is budgeted to stay within L2.

## Build and run

Requires a C++20 compiler and GNU make on Linux (developed under WSL2).

```sh
make              # build benchmarks and tools into build/bin
make run-bench    # build and execute every benchmark
make clean
```

Verify the host satisfies the ABI assumptions the zero-copy decoder depends on —
fixed integer widths, 64-bit pointers, little-endian byte order, padding-free packed
structs. Exits non-zero if any assumption is violated, so an unsupported target
fails loudly instead of decoding garbage:

```sh
./build/bin/host_probe            # full report
./build/bin/host_probe --quiet    # verify only; exit code is the result
```

Benchmarks emit machine-readable output for plotting:

```sh
./build/bin/memory_hierarchy --csv > results.csv
```

Interpreting benchmark output: judge it by **shape, not exact digits**. The last
level cache is shared across cores, so individual figures move run to run. What
must hold is that the latency curve steps upward in plateaus at this CPU's cache
capacities, that column-major traversal is dramatically slower than row-major, and
that the two checksums match.

## Layout

```
bench/    micro-benchmarks; each one backs a claim made in docs/
tools/    host inspection and verification utilities
include/  public headers
docs/     architecture rationale and benchmark write-ups
```

## License

MIT — see [LICENSE](LICENSE).
