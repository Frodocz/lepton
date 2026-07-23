# lepton

**lepton** is a lightweight, header-first C++20 network framework for building
low-latency network applications. It runs the same code over either the Linux
kernel (POSIX sockets, for dev/CI) or a **DPDK userspace TCP/IP stack via
[F-Stack](https://github.com/F-Stack/f-stack)** (for production, kernel-bypass),
selected at compile time. The design goal is a single value: **the lowest, most
predictable tail latency from the wire to your application logic.**

It is general-purpose — market data, telemetry ingest, real-time messaging, RPC
fan-in, or any workload that streams data over TCP/TLS/WebSocket and cares about
microseconds.

```
        NIC ──(DPDK / F-Stack)──▶ [reactor core] ──SPSC──▶ [consumer core]
                                       │  zero-copy WsMessageView
                                       ▼
                                  [logger core]  (async drain, off the hot path)
```

## Design principles

- **One reactor per core, share-nothing.** `EventLoop` is a single-threaded,
  busy-polling reactor (`event_loop.h`). No locks anywhere on the hot path —
  cross-core input enters only through the per-cycle step hook.
- **Compile-time backend seam.** Every syscall goes through `net::sys::*`
  (`sys_api.h`), so `TcpSocket`, `Poller`, and `EventLoop` are byte-identical for
  POSIX (`epoll`/`recv`) and F-Stack (`ff_epoll`/`ff_recv`). Flip `-DLEPTON_USE_FSTACK=ON`.
- **Zero-copy, zero-alloc hot path.** Received frames are handed to `on_message`
  as a `WsMessageView` that points *into* the session buffer. Outbound frames
  write their header *backwards* into reserved headroom (`IOBuffer::prepend`) so
  the payload is never copied; masking is done in place with an AVX2 fast path.
- **Pooled, pinned memory.** `BufferPool` pre-allocates all I/O buffers (DPDK
  `rte_mempool` under F-Stack; an `mlock`ed, pre-faulted, lock-free Treiber stack
  on POSIX). No `malloc`/`free`/page-fault on the hot path.
- **TSC timestamps.** `TscClock::tscns()` reads the invariant TSC instead of
  `clock_gettime` for nanosecond stamping on the hot path.
- **Transport as a concept.** Protocol layers are templated on a `Stream`
  concept, so `WsSession<TcpSocket>` (`ws://`) and `WsSession<TlsStream>`
  (`wss://`, OpenSSL over memory BIOs) share one state machine with zero runtime cost.

## Layout

| Path | What it is |
|------|-----------|
| `include/lepton/base/` | `BufferPool`, `IOBuffer`, `TscClock`, logger, endian, attributes |
| `include/lepton/net/`  | `EventLoop`, `TcpSocket`, `WsSession`, HTTP client, `Endpoint` |
| `include/lepton/net/detail/` | `Poller`, `sys_api` seam, WS frame codec / masking |
| `include/lepton/net/security/` | `TlsContext`, `TlsStream` (async OpenSSL) |
| `examples/` | runnable samples (see below) |
| `tests/` | unit + stress tests |

## Requirements

- A C++20 compiler (GCC 11+ / Clang 14+) and CMake ≥ 3.12.
- OpenSSL (for `TlsStream` / `wss://`).
- Quill is fetched automatically for the async logger.
- **For F-Stack mode only:** an installed [F-Stack](https://github.com/F-Stack/f-stack)
  + DPDK, hugepages, and a NIC bound to a DPDK poll-mode driver.

## Build (POSIX / kernel sockets)

The default backend uses the Linux kernel — no special privileges or hardware
setup. Good for development and CI.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Build and enable F-Stack (DPDK kernel-bypass)

F-Stack runs a FreeBSD-derived TCP/IP stack in userspace on top of DPDK, bypassing
the kernel for the lowest latency. Enabling it is a two-part process: prepare the
host once, then build lepton with the F-Stack backend.

### 1. Install and set up F-Stack + DPDK (once per host)

Follow the [F-Stack build guide](https://github.com/F-Stack/f-stack). In short:
build and install DPDK, then build and install F-Stack (this installs
`libfstack.a`, by default under `/usr/local/lib`). Then prepare the runtime host:

```bash
# Reserve hugepages (2 MiB pages shown; size to your workload).
sudo sh -c 'echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'
sudo mkdir -p /mnt/huge && sudo mount -t hugetlbfs nodev /mnt/huge

# Load the DPDK userspace I/O driver and bind your data-plane NIC to it
# (vfio-pci recommended). Replace the PCI address with your NIC's.
sudo modprobe vfio-pci
sudo dpdk-devbind.py --bind=vfio-pci 0000:00:06.0
```

### 2. Configure the F-Stack `.ini`

Edit `examples/f-stack.config.example.ini` to match your DPDK NIC: set the
`[port0]` `addr` / `netmask` / `broadcast` / `gateway` / `if_name` to the values
of the interface you bound above, and the `[dpdk]` `lcore_mask` to the core(s)
F-Stack should run on. `idle_sleep=0` and `pkt_tx_delay=0` (the defaults there)
keep it busy-polling for lowest latency.

### 3. Build lepton with the F-Stack backend

```bash
cmake -B build_fstack \
      -DLEPTON_USE_FSTACK=ON \
      -DFSTACK_LIB_DIR=/usr/local/lib \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build_fstack -j
```

`-DFSTACK_LIB_DIR` points at the directory containing `libfstack.a` (omit it if
you installed to the default `/usr/local/lib`).

## Run the best-practice example

`best_practice_example.cpp` is the recommended blueprint: pinned reactor /
consumer / logger cores, zero-copy receive, a wait-free SPSC handoff, correct
F-Stack shutdown, and **built-in latency benchmarking** — it logs the
connection-setup breakdown (TCP+TLS handshake, WebSocket upgrade, total
time-to-open) and prints a steady-state per-message latency percentile report on
exit, so you get a clear latency overview straight from the logs.

```bash
# POSIX build — runs against the kernel stack, no extra privileges needed.
./build/examples/best_practice_example

# F-Stack build — needs root (DPDK) and the F-Stack config via --conf.
sudo ./build_fstack/examples/best_practice_example \
     --conf examples/f-stack.config.example.ini
```

Example output:

```
[conn] TCP+TLS handshake: 11406 us
[conn] WS upgrade: 181178 us | TOTAL time-to-open: 192585 us
subscribed; streaming for 30s...
  REPORT: STEADY-STATE MESSAGE PIPELINE (samples: 152)
  Percentile | Reactor parse+enqueue | Inter-core handoff
  Min        |            14 ns      |         75 ns
  50% (med)  |            92 ns      |        207 ns
  90%        |           217 ns      |        298 ns
  99%        |          9111 ns      |        574 ns
  Max        |          9733 ns      |        688 ns
```

## Other examples

- `fstack_busy_poll_example.cpp`, `multi_threaded_busy_poll_example.cpp` —
  end-to-end streaming pipelines with per-channel latency reports.
- `ws_example` / `wss_example` / `http_example` / `https_example` — protocol basics.
- `*_logging.cpp` — coexisting the async logger with spdlog / fmtlog / Quill.

## Testing

```bash
cd build && ctest --output-on-failure          # POSIX

# F-Stack tests must run as root, one at a time (DPDK is a PRIMARY process), and
# take the config on the command line, e.g.:
sudo ./build_fstack/tests/test_buffer_pool --conf examples/f-stack.config.example.ini
```

## Performance notes

- **System tuning gives the biggest wins:** isolate the reactor/consumer cores
  (`isolcpus`, `nohz_full`, `rcu_nocbs`), keep them off the NIC IRQ core, and
  disable deep C-states and frequency scaling. Use hugepages for the buffer pool.
- **In lepton:** busy-poll mode (`EventLoopConfig::busy_poll = true`) removes the
  `epoll` syscall from the loop; `TCP_NODELAY` is set on connect; the TSC clock,
  AVX2 masking, and pooled buffers keep the hot path allocation- and syscall-free.

## License

See [LICENSE](LICENSE).
