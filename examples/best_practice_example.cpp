/// @file best_practice_example.cpp
/// @brief Canonical low-latency streaming client built on lepton, with built-in
///        latency benchmarking.
///
/// This is the reference "how to wire lepton for the lowest, most predictable
/// latency" example. It distils the patterns from fstack_busy_poll_example.cpp /
/// multi_threaded_busy_poll_example.cpp into the recommended shape, annotates
/// *why* each choice matters, and — folding in the benchmarking from the F-Stack
/// busy-poll example — measures and reports latency so you get a clear overview
/// straight from the logs:
///
///   * Connection-setup latency, broken down into TCP+TLS handshake, WebSocket
///     upgrade, and total time-to-open (logged as each phase completes).
///   * Steady-state per-message latency: reactor parse/enqueue time and the
///     inter-core handoff, summarised as a percentile report on shutdown.
///
/// It connects to a public WebSocket endpoint purely as a convenient live data
/// source; nothing here is domain-specific. Swap the host/path/subscription for
/// your own feed.
///
/// ─────────────────────────────────────────────────────────────────────────────
/// Architecture (share-nothing, one job per core)
/// ─────────────────────────────────────────────────────────────────────────────
///
///     NIC ──(DPDK/F-Stack)──▶  [Core R: reactor]  ──SPSC──▶  [Core C: consumer]
///                                   │                              │
///                                   │  zero-copy WsMessageView     │  app logic
///                                   │  parse → copy into slot      │  (never blocks R)
///                                   ▼                              ▼
///                              [Core L: logger backend drain]
///
///   * Core R (the F-Stack thread) busy-polls the WebSocket session. It never
///     sleeps, never locks, never allocates on the hot path, and hands off to the
///     consumer core through a wait-free SPSC ring.
///   * Core C runs your application logic. Decoupling it from R means a slow
///     consumer tick can never add jitter to packet reception.
///   * Core L drains the (async) logger backend so LEPTON_LOG_* on R/C only ever
///     does a wait-free enqueue — formatting/IO happen off the hot path.
///
/// Golden rules this example demonstrates:
///   1. One reactor per core, busy-poll, pinned, share-nothing (no locks).
///   2. Zero-copy receive: WsMessageView points into the session buffer; copy the
///      few bytes you keep straight into an SPSC slot (never std::string).
///   3. Pool all I/O buffers up front (mlock + pre-fault) — no page faults, no
///      malloc/free on the hot path.
///   4. TSC timestamps (tscns) instead of clock_gettime on the hot path.
///   5. Tear DPDK-backed resources down inside the loop's shutdown hook, on the
///      loop thread, while the EAL is still alive.

#include "lepton/base/logger.h"
#include "lepton/base/tsc_clock.h"
#include "lepton/init.h"
#include "lepton/net/event_loop.h"
#include "lepton/net/ws_session.h"
#include "lepton/net/security/tls_context.h"
#include "lepton/net/security/tls_stream.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>

using namespace lepton;

// ── Core assignment ──────────────────────────────────────────────────────────
// Pin each role to its own physical core. In production also isolate these cores
// from the scheduler (kernel cmdline: isolcpus=1,2,3 nohz_full=1,2,3 rcu_nocbs=1,2,3)
// and keep them off the NIC's IRQ core.
namespace core {
constexpr int kReactor  = 1;  // F-Stack / busy-poll reactor
constexpr int kConsumer = 2;  // application logic (SPSC consumer)
constexpr int kLogger   = 3;  // async logger backend drain
}  // namespace core

inline void pin_to_core(int core_id) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core_id, &set);
    if (::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) != 0) {
        LEPTON_LOG_WARN("failed to pin thread to core {}", core_id);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Wait-free SPSC ring (single producer = reactor, single consumer = consumer).
// Meng Rao style: cache the far index so the steady state touches only one
// cache line per side and never the peer's atomic. Fixed-size, no allocation.
// ─────────────────────────────────────────────────────────────────────────────
template <typename T, uint32_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
public:
    // Producer: reserve a slot (nullptr if full → drop or apply backpressure).
    T* alloc() noexcept {
        uint32_t w = w_.load(std::memory_order_relaxed);
        if (w - r_cache_ >= N) {
            r_cache_ = r_.load(std::memory_order_acquire);
            if (w - r_cache_ >= N) return nullptr;
        }
        return &data_[w & (N - 1)];
    }
    void commit() noexcept { w_.store(w_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

    // Consumer: peek the next filled slot (nullptr if empty), then release it.
    T* peek() noexcept {
        uint32_t r = r_.load(std::memory_order_relaxed);
        if (r == w_cache_) {
            w_cache_ = w_.load(std::memory_order_acquire);
            if (r == w_cache_) return nullptr;
        }
        return &data_[r & (N - 1)];
    }
    void release() noexcept { r_.store(r_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

private:
    alignas(64) std::atomic<uint32_t> w_{0};
    uint32_t r_cache_{0};
    alignas(64) std::atomic<uint32_t> r_{0};
    uint32_t w_cache_{0};
    alignas(64) T data_[N];
};

// The message we hand to the consumer core. Trivially copyable, carries only the
// snapshot the consumer needs plus TSC stamps for latency accounting.
struct Message {
    int64_t recv_ns;        // stamped the instant on_message fired (TSC)
    int64_t publish_ns;     // stamped right before commit() to the ring
    uint32_t len;           // payload length actually captured
    char    payload[192];   // small snapshot; consumer re-parses if it wants more
};

static SpscRing<Message, 8192> g_ring;
static std::atomic<bool> g_running{true};

// ─────────────────────────────────────────────────────────────────────────────
// Percentile report helper (sorts in place).
// ─────────────────────────────────────────────────────────────────────────────
static void print_latency_report(const char* title, std::vector<int64_t>& parse_ns,
                                  std::vector<int64_t>& handoff_ns) {
    if (handoff_ns.empty()) {
        std::printf("\nNo samples collected for: %s\n", title);
        return;
    }
    std::sort(parse_ns.begin(), parse_ns.end());
    std::sort(handoff_ns.begin(), handoff_ns.end());

    auto pct = [](const std::vector<int64_t>& v, double p) -> int64_t {
        if (v.empty()) return 0;
        return v[static_cast<std::size_t>(p * (v.size() - 1))];
    };

    std::printf("\n--------------------------------------------------------------------\n");
    std::printf("  REPORT: %s (samples: %zu)\n", title, handoff_ns.size());
    std::printf("--------------------------------------------------------------------\n");
    std::printf("  Percentile | Reactor parse+enqueue | Inter-core handoff\n");
    std::printf("  -----------+-----------------------+-------------------\n");
    std::printf("  Min        | %13ld ns       | %10ld ns\n", parse_ns.front(), handoff_ns.front());
    std::printf("  50%% (med)  | %13ld ns       | %10ld ns\n", pct(parse_ns, 0.50), pct(handoff_ns, 0.50));
    std::printf("  90%%        | %13ld ns       | %10ld ns\n", pct(parse_ns, 0.90), pct(handoff_ns, 0.90));
    std::printf("  99%%        | %13ld ns       | %10ld ns\n", pct(parse_ns, 0.99), pct(handoff_ns, 0.99));
    std::printf("  Max        | %13ld ns       | %10ld ns\n", parse_ns.back(), handoff_ns.back());
    std::printf("--------------------------------------------------------------------\n");
    std::fflush(stdout);
}

int main(int argc, char* argv[]) {
    // ── 1. Async logger. init_logger() only sets up the frontend; a dedicated
    //       thread (core L) binds and drains the backend so hot-path logging is
    //       just a wait-free enqueue.
    lepton::init_logger({.level = lepton::LogLevel::Info, .to_console = true});

    std::jthread logger_thread([](std::stop_token st) {
        pin_to_core(core::kLogger);
        lepton::PollLoggerScope scope;               // start/stop backend on THIS thread
        while (!st.stop_requested()) {
            lepton::poll_logger_for(50);             // drain up to 50us worth, then loop
        }
    });

    TscClock::calibrate();  // calibrate rdtsc→ns once, up front

    // TLS context + endpoint are EAL-independent, so build them on main.
    security::TlsContext tls_ctx(security::TlsContext::Options{.verify_peer = false});
    auto ep = net::Endpoint{}.resolve("ws.okx.com", 8443);
    if (!ep) {
        LEPTON_LOG_ERROR("resolve failed");
        return 1;
    }

    // ── 2. Consumer core: drain the ring, run app logic, collect latency samples.
    //       Never touches the network. ──
    std::vector<int64_t> parse_samples, handoff_samples;
    parse_samples.reserve(1u << 20);    // pre-size so push_back never allocates mid-run
    handoff_samples.reserve(1u << 20);

    std::thread consumer_thread([&] {
        pin_to_core(core::kConsumer);
        uint64_t n = 0;
        while (g_running.load(std::memory_order_relaxed)) {
            Message* m = g_ring.peek();
            if (!m) {
                __builtin_ia32_pause();  // brief spin; no yield → no scheduler jitter
                continue;
            }
            const int64_t now = TscClock::tscns();
            const int64_t handoff_ns = now - m->publish_ns;      // reactor → consumer
            const int64_t parse_ns   = m->publish_ns - m->recv_ns; // reactor-side work
            handoff_samples.push_back(handoff_ns);
            parse_samples.push_back(parse_ns);

            // ── your application logic goes here (m->payload, m->len) ──

            if ((++n % 500) == 0) {
                LEPTON_LOG_INFO("[consumer] msgs={} last_handoff={}ns last_parse={}ns",
                                n, handoff_ns, parse_ns);
            }
            g_ring.release();
        }
    });

    // ── 3. Reactor core (this thread). Under F-Stack everything that touches the
    //       EAL — init, pool, socket, loop — must live on this one thread. ──
    pin_to_core(core::kReactor);
    if (lepton::init(argc, argv, "best_practice") < 0) {
        LEPTON_LOG_ERROR("lepton::init failed");
        g_running = false;
        if (consumer_thread.joinable()) consumer_thread.join();
        return 1;
    }

    net::EventLoop loop(net::EventLoopConfig{.busy_poll = true});

    // Pool holds DPDK-backed buffers; keep it (and the session) in unique_ptrs so
    // the shutdown hook can free them on this thread before the EAL is torn down.
    auto pool = std::make_unique<BufferPool>(64, 8192, /*hugepage=*/false);
    auto ws = std::make_unique<net::WsSession<security::TlsStream>>(loop, *pool);
    ws->set_host("ws.okx.com");
    ws->set_path("/ws/v5/public");
    ws->set_sec_key("dGhlIHNhbXBsZSBub25jZQ==");
    ws->set_ping_interval_ns(15'000'000'000);
    ws->set_pong_timeout_ns(5'000'000'000);
    ws->transport().set_context(tls_ctx);
    ws->transport().set_hostname("ws.okx.com");

    // ── Connection-setup latency benchmark ──────────────────────────────────────
    // The session walks Connecting → Upgrading → Open. Stamping each transition
    // gives a breakdown of where connection time goes, logged as it happens:
    //   Connecting→Upgrading : TCP connect + TLS handshake
    //   Upgrading→Open       : HTTP WebSocket upgrade round-trip
    //   Connecting→Open      : total time-to-open
    struct ConnTiming {
        int64_t connect_start_ns{0};
        int64_t upgrading_ns{0};
    } timing;

    ws->on_state([&timing](net::WsState state) {
        const int64_t now = TscClock::tscns();
        switch (state) {
            case net::WsState::Upgrading:
                timing.upgrading_ns = now;
                LEPTON_LOG_INFO("[conn] TCP+TLS handshake: {} us",
                                (now - timing.connect_start_ns) / 1000);
                break;
            case net::WsState::Open:
                LEPTON_LOG_INFO("[conn] WS upgrade: {} us | TOTAL time-to-open: {} us",
                                (now - timing.upgrading_ns) / 1000,
                                (now - timing.connect_start_ns) / 1000);
                break;
            default:
                break;
        }
    });

    // Hot path: zero-copy receive → copy small snapshot into an SPSC slot.
    ws->on_message([](const net::WsMessageView& msg) {
        const int64_t recv_ns = TscClock::tscns();  // stamp arrival FIRST
        Message* slot = g_ring.alloc();
        if (!slot) return;  // ring full: consumer is behind → drop (never block reactor)
        slot->recv_ns = recv_ns;
        slot->len = static_cast<uint32_t>(std::min(msg.payload.size(), sizeof(slot->payload)));
        std::memcpy(slot->payload, msg.payload.data(), slot->len);
        slot->publish_ns = TscClock::tscns();  // stamp just before publishing
        g_ring.commit();
    });

    timing.connect_start_ns = TscClock::tscns();
    ws->connect(*ep);

    // Per-cycle hook: subscribe once open, and stop after a demo run.
    bool subscribed = false;
    int64_t start_ns = 0;
    loop.set_step_hook([&] {
        const int64_t now = TscClock::tscns();
        if (start_ns == 0) start_ns = now;
        if (ws->state() == net::WsState::Open && !subscribed) {
            static constexpr std::string_view sub =
                R"({"op":"subscribe","args":[{"channel":"bbo-tbt","instId":"BTC-USDT"},)"
                R"({"channel":"trades","instId":"BTC-USDT"}]})";
            (void)ws->send({reinterpret_cast<const uint8_t*>(sub.data()), sub.size()}, false);
            subscribed = true;
            LEPTON_LOG_INFO("subscribed; streaming for 30s...");
        }
        if (now - start_ns >= 30'000'000'000LL) {  // 30s demo
            ws->close();
            loop.stop();
        }
    });

    // Shutdown hook: runs on THIS thread after the last step() and before the EAL
    // is dismantled. Stop the consumer, print the latency report, then release
    // DPDK-backed resources here (session first, then pool).
    loop.set_shutdown_hook([&] {
        g_running = false;
        if (consumer_thread.joinable()) consumer_thread.join();
        print_latency_report("STEADY-STATE MESSAGE PIPELINE", parse_samples, handoff_samples);
        ws.reset();
        pool.reset();
        LEPTON_LOG_INFO("shutdown complete");
    });

    LEPTON_LOG_INFO("reactor running (busy-poll) on core {}", core::kReactor);
    loop.run();  // busy-poll until stop() (F-Stack pumps the PMD)

    // Under F-Stack the process may abort inside EAL teardown; the consumer join
    // and report already happened in the shutdown hook above.
    if (consumer_thread.joinable()) consumer_thread.join();
    return 0;
}
