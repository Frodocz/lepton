/// @file multi_threaded_busy_poll_example.cpp
/// @brief Ultra-low latency multi-threaded busy-polling reactor example.
///
/// Connects to OKX public WSS server to subscribe to BTC-USDT tickers:
/// 1. Market Data (MD) Thread: Pins to CPU Core 1, busy-polls the WSS connection,
///    receives ticker payloads, stamps the local receive time, parses exchange
///    timestamps, and pushes them to a Meng Rao style lock-free SPSC queue.
/// 2. Core Thread: Pins to CPU Core 2, busy-polls the SPSC queue, measures
///    the inter-thread handoff latency (nanoseconds) and full E2E exchange-to-core
///    latency (milliseconds), collecting statistics.
/// 3. Main Thread: Runs the show for 15 seconds, triggers shutdown, and prints
///    a comprehensive latency percentile table (10% to 99.9%).

#include "lepton/base/logger.h"
#include "lepton/base/tsc_clock.h"
#include "lepton/net/event_loop.h"
#include "lepton/net/ws_session.h"
#include "lepton/net/security/tls_context.h"
#include "lepton/net/security/tls_stream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>

using namespace lepton;

struct TickData {
    int64_t local_recv_ns;
    int64_t exchange_ns;
    double price;
    char symbol[16];
};

// -----------------------------------------------------------------------------
// Meng Rao Style High-Performance Lock-Free SPSC Queue (Contiguous Ring Buffer)
// -----------------------------------------------------------------------------
template <typename T, uint32_t CNT>
class SPSCQueue {
public:
    static_assert((CNT & (CNT - 1)) == 0, "Capacity (CNT) must be a power of 2");

    T* alloc() noexcept {
        uint32_t w = write_idx_.load(std::memory_order_relaxed);
        if (w - read_idx_cache_ >= CNT) {
            read_idx_cache_ = read_idx_.load(std::memory_order_acquire);
            if (w - read_idx_cache_ >= CNT) {
                return nullptr;
            }
        }
        return &data_[w & (CNT - 1)];
    }

    void push() noexcept {
        uint32_t w = write_idx_.load(std::memory_order_relaxed);
        write_idx_.store(w + 1, std::memory_order_release);
    }

    T* front() noexcept {
        uint32_t r = read_idx_.load(std::memory_order_relaxed);
        if (r == write_idx_cache_) {
            write_idx_cache_ = write_idx_.load(std::memory_order_acquire);
            if (r == write_idx_cache_) {
                return nullptr;
            }
        }
        return &data_[r & (CNT - 1)];
    }

    void pop() noexcept {
        uint32_t r = read_idx_.load(std::memory_order_relaxed);
        read_idx_.store(r + 1, std::memory_order_release);
    }

private:
    alignas(64) std::atomic<uint32_t> write_idx_{0};
    uint32_t write_idx_cache_{0};
    alignas(64) std::atomic<uint32_t> read_idx_{0};
    uint32_t read_idx_cache_{0};
    alignas(64) T data_[CNT];
};

static SPSCQueue<TickData, 4096> g_queue;
static std::atomic<bool> g_running{true};

// -----------------------------------------------------------------------------
// CPU Core Pinning Helper
// -----------------------------------------------------------------------------
inline void pin_thread_to_core(int core_id) noexcept {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if (::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpuset) == 0) {
        LEPTON_LOG_INFO("Successfully pinned thread to CPU Core {}", core_id);
    } else {
        LEPTON_LOG_WARN("Failed to set thread affinity for CPU Core {}", core_id);
    }
}

// -----------------------------------------------------------------------------
// Zero-Allocation JSON Parsing Helpers
// -----------------------------------------------------------------------------
inline int64_t parse_ts_from_json(std::string_view json) {
    auto pos = json.find("\"ts\":\"");
    if (pos == std::string_view::npos) return 0;
    pos += 6;
    int64_t val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        pos++;
    }
    return val * 1'000'000; // milliseconds to nanoseconds
}

inline double parse_price_from_json(std::string_view json) {
    auto pos = json.find("\"last\":\"");
    if (pos == std::string_view::npos) return 0.0;
    pos += 8;
    double val = 0.0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        pos++;
    }
    if (pos < json.size() && json[pos] == '.') {
        pos++;
        double dec = 0.1;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            val += (json[pos] - '0') * dec;
            dec *= 0.1;
            pos++;
        }
    }
    return val;
}

// -----------------------------------------------------------------------------
// Percentiles Statistic Helper
// -----------------------------------------------------------------------------
inline void print_latency_report(std::vector<int64_t>& handoff, std::vector<int64_t>& e2e) {
    if (handoff.empty()) {
        std::printf("\nNo latency samples collected.\n");
        return;
    }
    std::sort(handoff.begin(), handoff.end());

    // Filter out any invalid E2E latency values (< 0 or 0)
    std::vector<int64_t> valid_e2e;
    valid_e2e.reserve(e2e.size());
    for (auto val : e2e) {
        if (val > 0) {
            valid_e2e.push_back(val);
        }
    }
    std::sort(valid_e2e.begin(), valid_e2e.end());

    auto get_pct = [](const std::vector<int64_t>& v, double pct) -> double {
        if (v.empty()) return 0.0;
        std::size_t idx = static_cast<std::size_t>(pct * (v.size() - 1));
        return static_cast<double>(v[idx]);
    };

    double handoff_sum = 0;
    for (auto l : handoff) handoff_sum += l;
    double handoff_mean = handoff_sum / handoff.size();

    double e2e_sum = 0;
    for (auto l : valid_e2e) e2e_sum += l;
    double e2e_mean = !valid_e2e.empty() ? (e2e_sum / valid_e2e.size()) / 1'000'000.0 : 0.0;

    std::printf("\n===========================================================\n");
    std::printf("  LEPTON MULTI-THREADED LATENCY PERFORMANCE REPORT\n");
    std::printf("  Queue Type: MengRaoSPSCQueue (Flat Contiguous Ring)\n");
    std::printf("  Total Ticker Samples: %zu (Valid E2E: %zu)\n", handoff.size(), valid_e2e.size());
    std::printf("  Note: Exchange latency depends on local NTP clock sync.\n");
    std::printf("===========================================================\n");
    std::printf("  Percentile   | Queue Handoff Latency  | E2E Exchange Latency \n");
    std::printf("               | (MD -> Core Thread)    | (OKX -> Core Thread) \n");
    std::printf("  -------------+------------------------+----------------------\n");
    std::printf("  Min          |   %10.1f ns       |   %10.3f ms\n", (double)handoff.front(), get_pct(valid_e2e, 0.0) / 1'000'000.0);
    std::printf("  10%%          |   %10.1f ns       |   %10.3f ms\n", get_pct(handoff, 0.10), get_pct(valid_e2e, 0.10) / 1'000'000.0);
    std::printf("  20%%          |   %10.1f ns       |   %10.3f ms\n", get_pct(handoff, 0.20), get_pct(valid_e2e, 0.20) / 1'000'000.0);
    std::printf("  30%%          |   %10.1f ns       |   %10.3f ms\n", get_pct(handoff, 0.30), get_pct(valid_e2e, 0.30) / 1'000'000.0);
    std::printf("  40%%          |   %10.1f ns       |   %10.3f ms\n", get_pct(handoff, 0.40), get_pct(valid_e2e, 0.40) / 1'000'000.0);
    std::printf("  50%% (Median) |   %10.1f ns       |   %10.3f ms\n", get_pct(handoff, 0.50), get_pct(valid_e2e, 0.50) / 1'000'000.0);
    std::printf("  60%%          |   %10.1f ns       |   %10.3f ms\n", get_pct(handoff, 0.60), get_pct(valid_e2e, 0.60) / 1'000'000.0);
    std::printf("  70%%          |   %10.1f ns       |   %10.3f ms\n", get_pct(handoff, 0.70), get_pct(valid_e2e, 0.70) / 1'000'000.0);
    std::printf("  80%%          |   %10.1f ns       |   %10.3f ms\n", get_pct(handoff, 0.80), get_pct(valid_e2e, 0.80) / 1'000'000.0);
    std::printf("  90%%          |   %10.1f ns       |   %10.3f ms\n", get_pct(handoff, 0.90), get_pct(valid_e2e, 0.90) / 1'000'000.0);
    std::printf("  95%%          |   %10.1f ns       |   %10.3f ms\n", get_pct(handoff, 0.95), get_pct(valid_e2e, 0.95) / 1'000'000.0);
    std::printf("  99%%          |   %10.1f ns       |   %10.3f ms\n", get_pct(handoff, 0.99), get_pct(valid_e2e, 0.99) / 1'000'000.0);
    std::printf("  99.9%%        |   %10.1f ns       |   %10.3f ms\n", get_pct(handoff, 0.999), get_pct(valid_e2e, 0.999) / 1'000'000.0);
    std::printf("  Max          |   %10.1f ns       |   %10.3f ms\n", (double)handoff.back(), get_pct(valid_e2e, 1.0) / 1'000'000.0);
    std::printf("  Mean         |   %10.1f ns       |   %10.3f ms\n", handoff_mean, e2e_mean);
    std::printf("===========================================================\n");
}

int main() {
    lepton::init_logger({
        .level = lepton::LogLevel::Info,
        .to_console = true
    });
    lepton::PollScope logger_scope;

    TscClock::calibrate();

    // 1. Initialize EventLoop and security contexts
    net::EventLoopConfig loop_cfg{.busy_poll = true}; // High-frequency busy poll reactor
    net::EventLoop loop(loop_cfg);
    BufferPool pool(32, 4096, false);

    security::TlsContext::Options tls_opts{.verify_peer = false};
    security::TlsContext tls_ctx(tls_opts);

    // Resolve OKX Endpoint
    auto ep_opt = net::Endpoint{}.resolve("ws.okx.com", 8443);
    if (!ep_opt) {
        LEPTON_LOG_ERROR("Failed to resolve ws.okx.com");
        return 1;
    }
    net::Endpoint ep = *ep_opt;

    // 2. Setup WsSession
    net::WsSession<security::TlsStream> ws(loop, pool);
    ws.set_host("ws.okx.com");
    ws.set_path("/ws/v5/public");
    ws.set_sec_key("dGhlIHNhbXBsZSBub25jZQ==");
    ws.set_ping_interval_ns(15'000'000'000);   // 15s ping
    ws.set_pong_timeout_ns(5'000'000'000);      // 5s pong

    ws.transport().set_context(tls_ctx);
    ws.transport().set_hostname("ws.okx.com");

    std::atomic<bool> subscribed{false};

    ws.on_state([](net::WsState state) {
        LEPTON_LOG_INFO("MD WSS Connection State: {}", static_cast<int>(state));
    });

    ws.on_message([&subscribed, &ws](const net::WsMessageView& msg) {
        int64_t local_recv = TscClock::tscns();
        std::string_view payload{reinterpret_cast<const char*>(msg.payload.data()), msg.payload.size()};

        // Filter for ticker message payload
        if (payload.find("\"tickers\"") != std::string_view::npos) {
            int64_t exchange_ts = parse_ts_from_json(payload);
            if (exchange_ts > 0) {
                double price = parse_price_from_json(payload);
                TickData* slot = g_queue.alloc();
                if (slot) {
                    slot->local_recv_ns = local_recv;
                    slot->exchange_ns = exchange_ts;
                    slot->price = price;
                    std::memcpy(slot->symbol, "BTC-USDT", 9);
                    g_queue.push();
                }
            }
        }
    });

    // 3. Spawn Market Data (MD) Thread
    std::thread md_thread([&ws, &loop, &subscribed, ep]() {
        pin_thread_to_core(1);
        ws.connect(ep);

        while (g_running) {
            loop.step();

            if (ws.state() == net::WsState::Open && !subscribed) {
                LEPTON_LOG_INFO("WSS connected. Subscribing to BTC-USDT tickers...");
                std::string sub_json = R"({"op": "subscribe", "args": [{"channel": "tickers", "instId": "BTC-USDT"}]})";
                (void)ws.send({reinterpret_cast<const uint8_t*>(sub_json.data()), sub_json.size()}, /*binary=*/false);
                subscribed = true;
            }
            std::this_thread::yield();
        }
        ws.close();
        loop.step();
    });

    // 4. Spawn Core Thread (Collects Tick and Latency Statistics)
    std::vector<int64_t> handoff_latencies;
    std::vector<int64_t> e2e_latencies;
    handoff_latencies.reserve(5000);
    e2e_latencies.reserve(5000);

    std::thread core_thread([&handoff_latencies, &e2e_latencies]() {
        pin_thread_to_core(2);

        while (g_running) {
            TickData* tick = g_queue.front();
            if (tick) {
                int64_t now = TscClock::tscns();
                int64_t handoff_ns = now - tick->local_recv_ns;
                int64_t e2e_ns = tick->exchange_ns > 0 ? (now - tick->exchange_ns) : 0;

                handoff_latencies.push_back(handoff_ns);
                e2e_latencies.push_back(e2e_ns);

                if (handoff_latencies.size() % 20 == 0) {
                    LEPTON_LOG_INFO("[Core] Ticker BTC-USDT Price: {:.2f} | Handoff: {} ns",
                                    tick->price, handoff_ns);
                }

                g_queue.pop();
            } else {
                std::this_thread::yield();
            }
        }
    });

    // Let it run for 15 seconds to gather realistic market data feeds
    LEPTON_LOG_INFO("Running multi-threaded busy-polling pipeline for 15 seconds...");
    std::this_thread::sleep_for(std::chrono::seconds(15));

    // Trigger shutdown
    g_running = false;
    md_thread.join();
    core_thread.join();

    // Print percentile statistical report
    print_latency_report(handoff_latencies, e2e_latencies);

    return 0;
}
