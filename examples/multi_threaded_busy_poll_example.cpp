/// @file multi_threaded_busy_poll_example.cpp
/// @brief Ultra-low latency multi-threaded busy-polling reactor example.
///
/// Connects to OKX public WSS server to subscribe to BTC-USDT tickers, BBO, and trades:
/// 1. Market Data (MD) Thread: Pins to CPU Core 1, busy-polls the WSS connection,
///    receives ticker, BBO, and trade payloads, stamps raw receive time, parses JSON,
///    stamps post-parse time, stamps push time, and pushes to a lock-free SPSC queue.
/// 2. Core Thread: Pins to CPU Core 2, busy-polls the SPSC queue, measures
///    the inter-thread handoff latency (ns), JSON parse latency (ns), network
///    receive latency (ms), and full E2E latency (ms), collecting statistics per channel.
/// 3. Main Thread: Runs for 15 seconds, triggers shutdown, and prints reports.

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
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>

using namespace lepton;

enum class ChannelType : uint8_t {
    Ticker,
    BBO,
    Trade
};

struct TickData {
    ChannelType chan;
    int64_t exchange_ns;
    int64_t local_recv_ns;  // Stamped when on_message is called
    int64_t parse_done_ns;  // Stamped when JSON parsing is complete
    int64_t push_ns;        // Stamped right before SPSC push
    double price;
    char symbol[16];
};

// -----------------------------------------------------------------------------
// Meng Rao Style High-Performance Lock-Free SPSC Queue
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

inline double parse_price_from_json(std::string_view json, ChannelType chan) {
    std::string_view match_key = "\"last\":\"";
    if (chan == ChannelType::BBO) {
        // BBO format asks/bids arrays: bids: [["price", "size", ...]]
        // Let's grab the first double value in asks/bids or fallback
        match_key = "\"bids\":[[\\\"";
        // Alternatively, scan bids/asks starting bracket
        auto bids_pos = json.find("\"bids\":[[");
        if (bids_pos != std::string_view::npos) {
            auto pos = bids_pos + 9;
            if (pos < json.size() && json[pos] == '"') pos++;
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
        return 0.0;
    } else if (chan == ChannelType::Trade) {
        match_key = "\"px\":\"";
    }

    auto pos = json.find(match_key);
    if (pos == std::string_view::npos) return 0.0;
    pos += match_key.size();
    if (pos < json.size() && json[pos] == '"') pos++;
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
inline void print_single_report(const char* title, std::vector<int64_t>& handoff, std::vector<int64_t>& e2e,
                                std::vector<int64_t>& parse, std::vector<int64_t>& net_recv) {
    if (handoff.empty()) {
        std::printf("\nNo latency samples collected for: %s\n", title);
        return;
    }
    std::sort(handoff.begin(), handoff.end());
    std::sort(parse.begin(), parse.end());

    std::vector<int64_t> valid_e2e;
    std::vector<int64_t> valid_net;
    valid_e2e.reserve(e2e.size());
    valid_net.reserve(net_recv.size());
    for (auto val : e2e) if (val > 0) valid_e2e.push_back(val);
    for (auto val : net_recv) if (val > 0) valid_net.push_back(val);

    std::sort(valid_e2e.begin(), valid_e2e.end());
    std::sort(valid_net.begin(), valid_net.end());

    auto get_pct = [](const std::vector<int64_t>& v, double pct) -> double {
        if (v.empty()) return 0.0;
        std::size_t idx = static_cast<std::size_t>(pct * (v.size() - 1));
        return static_cast<double>(v[idx]);
    };

    double handoff_mean = 0.0; for (auto l : handoff) handoff_mean += l; handoff_mean /= handoff.size();
    double parse_mean = 0.0; for (auto l : parse) parse_mean += l; parse_mean /= parse.size();
    double e2e_mean = 0.0; for (auto l : valid_e2e) e2e_mean += l; e2e_mean = !valid_e2e.empty() ? (e2e_mean / valid_e2e.size()) : 0.0;
    double net_mean = 0.0; for (auto l : valid_net) net_mean += l; net_mean = !valid_net.empty() ? (net_mean / valid_net.size()) : 0.0;

    std::printf("\n-------------------------------------------------------------------------------------------\n");
    std::printf("  REPORT: %s (Samples: %zu)\n", title, handoff.size());
    std::printf("-------------------------------------------------------------------------------------------\n");
    std::printf("  Percentile   | Net-to-Receive   | JSON Parse Latency  | Queue Handoff   | E2E Exchange-to-Core\n");
    std::printf("               | (Exchange->Recv) | (MD Parsing time)   | (MD->Core Hand) | (Exchange->Processed)\n");
    std::printf("  -------------+------------------+---------------------+-----------------+---------------------\n");
    std::printf("  Min          |   %8.3f ms   |   %8.1f ns      |   %6.1f ns     |   %8.3f ms\n",
                get_pct(valid_net, 0.0) / 1'000'000.0, (double)parse.front(), (double)handoff.front(), get_pct(valid_e2e, 0.0) / 1'000'000.0);
    std::printf("  10%%          |   %8.3f ms   |   %8.1f ns      |   %6.1f ns     |   %8.3f ms\n",
                get_pct(valid_net, 0.10) / 1'000'000.0, get_pct(parse, 0.10), get_pct(handoff, 0.10), get_pct(valid_e2e, 0.10) / 1'000'000.0);
    std::printf("  20%%          |   %8.3f ms   |   %8.1f ns      |   %6.1f ns     |   %8.3f ms\n",
                get_pct(valid_net, 0.20) / 1'000'000.0, get_pct(parse, 0.20), get_pct(handoff, 0.20), get_pct(valid_e2e, 0.20) / 1'000'000.0);
    std::printf("  50%% (Median) |   %8.3f ms   |   %8.1f ns      |   %6.1f ns     |   %8.3f ms\n",
                get_pct(valid_net, 0.50) / 1'000'000.0, get_pct(parse, 0.50), get_pct(handoff, 0.50), get_pct(valid_e2e, 0.50) / 1'000'000.0);
    std::printf("  90%%          |   %8.3f ms   |   %8.1f ns      |   %6.1f ns     |   %8.3f ms\n",
                get_pct(valid_net, 0.90) / 1'000'000.0, get_pct(parse, 0.90), get_pct(handoff, 0.90), get_pct(valid_e2e, 0.90) / 1'000'000.0);
    std::printf("  99%%          |   %8.3f ms   |   %8.1f ns      |   %6.1f ns     |   %8.3f ms\n",
                get_pct(valid_net, 0.99) / 1'000'000.0, get_pct(parse, 0.99), get_pct(handoff, 0.99), get_pct(valid_e2e, 0.99) / 1'000'000.0);
    std::printf("  Max          |   %8.3f ms   |   %8.1f ns      |   %6.1f ns     |   %8.3f ms\n",
                get_pct(valid_net, 1.0) / 1'000'000.0, (double)parse.back(), (double)handoff.back(), get_pct(valid_e2e, 1.0) / 1'000'000.0);
    std::printf("  Mean         |   %8.3f ms   |   %8.1f ns      |   %6.1f ns     |   %8.3f ms\n",
                net_mean / 1'000'000.0, parse_mean, handoff_mean, e2e_mean / 1'000'000.0);
}

#include "lepton/init.h"

int main(int argc, char* argv[]) {
    lepton::init_logger({
        .level = lepton::LogLevel::Info,
        .to_console = true
    });

    if (lepton::init(argc, argv, "multi_threaded_busy_poll_example") < 0) {
        return 1;
    }

    std::jthread logger_thread([](std::stop_token stoken) {
        pin_thread_to_core(3);
        lepton::PollLoggerScope scope;
        while (!stoken.stop_requested()) {
            lepton::poll_logger_for(50);
        }
    });

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

        ChannelType chan = ChannelType::Ticker;
        bool matched = false;

        if (payload.find("\"channel\":\"tickers\"") != std::string_view::npos) {
            chan = ChannelType::Ticker;
            matched = true;
        } else if (payload.find("\"channel\":\"bbo-tbt\"") != std::string_view::npos) {
            chan = ChannelType::BBO;
            matched = true;
        } else if (payload.find("\"channel\":\"trades\"") != std::string_view::npos) {
            chan = ChannelType::Trade;
            matched = true;
        }

        if (matched) {
            int64_t exchange_ts = parse_ts_from_json(payload);
            if (exchange_ts > 0) {
                double price = parse_price_from_json(payload, chan);
                int64_t parse_done = TscClock::tscns();
                TickData* slot = g_queue.alloc();
                if (slot) {
                    slot->chan = chan;
                    slot->exchange_ns = exchange_ts;
                    slot->local_recv_ns = local_recv;
                    slot->parse_done_ns = parse_done;
                    slot->push_ns = TscClock::tscns();
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
                LEPTON_LOG_INFO("WSS connected. Subscribing to BTC-USDT tickers, bbo-tbt, and trades...");
                std::string sub_json = R"({"op": "subscribe", "args": [)"
                                       R"({"channel": "tickers", "instId": "BTC-USDT"},)"
                                       R"({"channel": "bbo-tbt", "instId": "BTC-USDT"},)"
                                       R"({"channel": "trades", "instId": "BTC-USDT"}])"
                                       R"(})";
                (void)ws.send({reinterpret_cast<const uint8_t*>(sub_json.data()), sub_json.size()}, /*binary=*/false);
                subscribed = true;
            }
            std::this_thread::yield();
        }
        ws.close();
        loop.step();
    });

    // 4. Spawn Core Thread (Collects Tick and Latency Statistics)
    // Tickers
    std::vector<int64_t> ticker_handoff, ticker_e2e, ticker_parse, ticker_net_recv;
    // BBO
    std::vector<int64_t> bbo_handoff, bbo_e2e, bbo_parse, bbo_net_recv;
    // Trades
    std::vector<int64_t> trade_handoff, trade_e2e, trade_parse, trade_net_recv;
    // Combined
    std::vector<int64_t> comb_handoff, comb_e2e, comb_parse, comb_net_recv;

    std::thread core_thread([&]() {
        pin_thread_to_core(2);

        while (g_running) {
            TickData* tick = g_queue.front();
            if (tick) {
                int64_t now = TscClock::tscns();
                int64_t handoff_ns = now - tick->push_ns;
                int64_t e2e_ns = tick->exchange_ns > 0 ? (now - tick->exchange_ns) : 0;
                int64_t parse_ns = tick->parse_done_ns - tick->local_recv_ns;
                int64_t net_recv_ns = tick->exchange_ns > 0 ? (tick->local_recv_ns - tick->exchange_ns) : 0;

                // Push to Combined
                comb_handoff.push_back(handoff_ns);
                comb_e2e.push_back(e2e_ns);
                comb_parse.push_back(parse_ns);
                comb_net_recv.push_back(net_recv_ns);

                // Push to specific channel vectors
                if (tick->chan == ChannelType::Ticker) {
                    ticker_handoff.push_back(handoff_ns);
                    ticker_e2e.push_back(e2e_ns);
                    ticker_parse.push_back(parse_ns);
                    ticker_net_recv.push_back(net_recv_ns);

                    if (ticker_handoff.size() % 20 == 0) {
                        LEPTON_LOG_INFO("[Core] [Ticker] Price: {:.2f} | Handoff: {} ns | Parse: {} ns | Net-Recv: {:.3f} ms",
                                        tick->price, handoff_ns, parse_ns, (double)net_recv_ns / 1'000'000.0);
                    }
                } else if (tick->chan == ChannelType::BBO) {
                    bbo_handoff.push_back(handoff_ns);
                    bbo_e2e.push_back(e2e_ns);
                    bbo_parse.push_back(parse_ns);
                    bbo_net_recv.push_back(net_recv_ns);

                    if (bbo_handoff.size() % 20 == 0) {
                        LEPTON_LOG_INFO("[Core] [BBO] Best Bid Price: {:.2f} | Handoff: {} ns | Parse: {} ns | Net-Recv: {:.3f} ms",
                                        tick->price, handoff_ns, parse_ns, (double)net_recv_ns / 1'000'000.0);
                    }
                } else if (tick->chan == ChannelType::Trade) {
                    trade_handoff.push_back(handoff_ns);
                    trade_e2e.push_back(e2e_ns);
                    trade_parse.push_back(parse_ns);
                    trade_net_recv.push_back(net_recv_ns);

                    if (trade_handoff.size() % 20 == 0) {
                        LEPTON_LOG_INFO("[Core] [Trade] Match Price: {:.2f} | Handoff: {} ns | Parse: {} ns | Net-Recv: {:.3f} ms",
                                        tick->price, handoff_ns, parse_ns, (double)net_recv_ns / 1'000'000.0);
                    }
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

    // Print percentile reports
    std::printf("\n\n===========================================================================================");
    std::printf("\n  LEPTON MULTI-CHANNEL LATENCY PERFORMANCE REPORT (MengRaoSPSCQueue)");
    std::printf("\n===========================================================================================\n");
    print_single_report("TICKERS CHANNEL", ticker_handoff, ticker_e2e, ticker_parse, ticker_net_recv);
    print_single_report("BBO-TBT CHANNEL", bbo_handoff, bbo_e2e, bbo_parse, bbo_net_recv);
    print_single_report("TRADES CHANNEL", trade_handoff, trade_e2e, trade_parse, trade_net_recv);
    print_single_report("COMBINED PIPELINE (ALL CHANNELS)", comb_handoff, comb_e2e, comb_parse, comb_net_recv);
    std::printf("===========================================================================================\n");

    return 0;
}
