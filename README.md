# Ultra-Low-Latency Kernel-Bypass Feed Handler

A kernel-bypass market data pipeline in C++20: an emulated PCIe NIC, a poll-mode
user-space driver, a zero-copy ITCH 5.0 decoder, and a price-level order book —
with every latency claim measured rather than asserted.

The receive path performs no allocation, no copy, and no syscall.

---

## Why

On a conventional Linux socket path, a UDP datagram crosses the driver, softirq
context, the network stack, a socket buffer, and a `recvmsg` syscall before the
application sees a byte — microseconds spent copying and switching privilege
levels rather than doing work. Kernel bypass deletes those layers: the device
writes payloads directly into application-owned memory and the application polls
for them.

Production kernel bypass requires a supported NIC bound to `vfio-pci`, an IOMMU,
and hugepages. None of that is available on this host, so the device is emulated:
`nicsim` presents a register file, descriptor rings, and doorbells with the same
contract a real device offers. The hardware is not reproduced; the engineering
above it — ring ownership, memory ordering between two agents on shared memory,
buffer lifetime, poll-mode operation — is, and the device layer stays swappable.

A conventional kernel receive path is retained as a control, so bypass numbers are
always quoted against a real baseline on the same machine.

Full rationale: [docs/architecture.md](docs/architecture.md).

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
| 32 KiB | L1d | 2.65 | 1.0x |
| 256 KiB | L2 | 7.01 | 2.6x |
| 4 MiB | L3 | 29.21 | 11x |
| 128 MiB | DRAM | 171.56 | **65x** |

The steps fall exactly on this CPU's documented cache capacities — the curve is
the hierarchy measuring itself.

**Cost of access order alone.** A 64 MiB matrix summed row-major and column-major:
identical arithmetic, identical bytes, only the order differs.

| order | ns / element |
|---|---:|
| row-major | 0.30 |
| column-major | 11.21 (**37.3x**) |

Together these set the project's priorities: a DRAM miss costs more than a hundred
arithmetic operations, so the decode path's memory layout matters far more than
its instruction count, and the hot working set is budgeted to stay within L2.

## Build

Requires a C++20 compiler and GNU make on Linux (developed under WSL2).

```sh
make              # build benchmarks and tools into build/bin
make run-bench    # build and execute every benchmark
make clean
```

Verify the host satisfies the ABI assumptions the zero-copy decoder depends on —
fixed integer widths, 64-bit pointers, little-endian byte order, padding-free
packed structs. Exits non-zero if any assumption is violated, so an unsupported
target fails loudly instead of decoding garbage:

```sh
./build/bin/host_probe
```

## Layout

```
bench/    micro-benchmarks; each one backs a claim made in docs/
tools/    host inspection and verification utilities
include/  public headers
docs/     architecture rationale and benchmark write-ups
```

## License

MIT — see [LICENSE](LICENSE).
