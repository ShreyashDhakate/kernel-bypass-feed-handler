# Architecture

## Problem

A market data feed handler's job is to turn photons on a wire into an updated
order book, and to do it before anyone else does. On a conventional Linux socket
path, a UDP datagram crosses the kernel's driver, softirq, network stack, socket
buffer, and a `recvmsg` syscall before user code ever sees it — several
microseconds of latency, most of it spent copying bytes and switching privilege
levels rather than doing work the application cares about.

Kernel bypass removes those layers: the NIC writes packet payloads directly into
memory the application already owns, and the application polls for them. There is
no syscall on the data path, no kernel copy, and no interrupt.

## Constraint, and the way around it

Real kernel bypass needs a supported NIC (Intel X710, Mellanox ConnectX,
Solarflare) bound to `vfio-pci`, an IOMMU, and hugepages. That hardware is not
available here, and none of it exists under WSL2.

So the device itself is emulated. `nicsim` is a user-space process that behaves
like a PCIe network device: it exposes a register file with the semantics of a
BAR, consumes descriptors from rings in shared memory, writes packet payloads
into buffers by DMA-equivalent copies, and signals completion through a doorbell
and a completion counter rather than an interrupt.

This is a deliberate trade. What is *not* reproduced is the hardware: PCIe
transaction ordering, real DMA, IOMMU translation, and true MMIO write latency.
What *is* reproduced is everything the driver above it must get right — descriptor
ring ownership, producer/consumer index discipline, memory ordering between two
agents observing shared memory, doorbell batching, zero-copy buffer lifetime, and
poll-mode operation. The driver is written against the same contract a real
device offers, so the device layer stays swappable: replacing `nicsim` with a
VFIO-backed device should not change the ring logic above it.

A conventional `AF_PACKET`/`AF_XDP` receive path is kept alongside as a control,
so the bypass path's numbers are always quoted against a real kernel path measured
on the same host.

## Pipeline

The full architecture — the two receive paths compared, system decomposition,
the RX hot path as a sequence with its memory-ordering requirements, descriptor
ring mechanics, packet layout to the byte, core topology, and the latency budget —
is diagrammed in the [README](../README.md#architecture).

It is maintained there rather than duplicated here, so there is exactly one
description of the system and it cannot drift.

## Design commitments

These follow from the measurements in [benchmarks/memory-hierarchy.md](benchmarks/memory-hierarchy.md),
where a DRAM access costs 54–66x an L1 hit and traversal order alone moved
identical work by 37x.

- **No allocation on the hot path.** Every buffer is drawn from a pool sized at
  startup. `new`, `malloc`, and any container that might grow are confined to
  initialisation.
- **No syscalls on the hot path.** Receive is a poll of a memory location.
- **Zero copy.** A frame is decoded in the buffer the device wrote it into.
  Nothing is memcpy'd out of it before the book is updated.
- **Hot working set bounded by L2.** Ring sizes, buffer pool extent, and the
  book's resident price levels are chosen so the steady-state footprint stays
  inside 512 KiB.
- **No shared cache lines between threads.** Producer and consumer indices sit on
  separate lines; anything crossing a thread boundary is padded and its memory
  ordering is stated explicitly.
- **Every claim is measured.** Latency is reported as a distribution with tail
  percentiles, against the kernel path as a control, on a named host.
