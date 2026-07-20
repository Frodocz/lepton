#pragma once

#define QUILL_DISABLE_NON_PREFIXED_MACROS

#include "lepton/base/attributes.h"

#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/bundled/fmt/format.h>

#include <cassert>

// Debug-only guard: fires a clear message if a LEPTON_LOG_* is used before
// lepton::init_logger() set the root logger. Compiles out entirely under
// -DNDEBUG (release)
#define LEPTON_ASSERT_LOGGER_READY() \
    assert(::lepton::lepton_root_logger != nullptr && \
           "lepton::init_logger() must be called before any LEPTON_LOG_* macro")

namespace lepton {

extern LEPTON_API quill::Logger* lepton_root_logger;

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------
// lepton's own log-level enum so users never need to name quill:: types.
enum class LogLevel : uint8_t {
    Debug,
    Info,
    Warning,
    Error,
};

// Default runtime level, chosen by the CONSUMER's build type: Debug builds get
// verbose Debug logs, optimized builds (NDEBUG defined) start at Info. Callers
// can always override LoggerConfig::level explicitly.
#if defined(NDEBUG)
inline constexpr LogLevel kDefaultLogLevel{LogLevel::Info};
#else
inline constexpr LogLevel kDefaultLogLevel{LogLevel::Debug};
#endif

// Callers set only what they care about and extensible for new options in the future.
// The two sink flags are independent: enable either or both.
struct LoggerConfig {
    // Defaults to Debug in Debug builds, Info in optimized (NDEBUG) builds.
    LogLevel level{kDefaultLogLevel};

    // --- Sink selection (independent; enable either or both) ---
    bool to_console{false};       // stdout, colorized — good for local/dev tails
    bool to_rotating_file{true};  // bounded on-disk logs — default for servers

    // --- RotatingFile options (ignored when to_rotating_file is false) ---
    // Base filename. Rotated files derive from this (e.g. lepton.1.log, ...).
    std::string filename{"lepton.log"};
    // Roll to a new file once the current one reaches this many bytes.
    std::size_t max_file_size_bytes{128 * 1024 * 1024};  // 128 MiB
    // Keep at most this many rotated files; older ones are removed. Bounds disk
    // usage for an infinite-loop process. 0 means "no count limit".
    std::uint32_t max_backup_files{10};
    // Also roll daily at this HH:MM (GmtTime). Empty disables time-based rotation.
    std::string daily_rotation_time{"00:00"};

    // --- Formatting ---
    // Quill pattern for the log line. Empty uses lepton's default pattern.
    std::string pattern{};
};

// -----------------------------------------------------------------------------
// Lifecycle management
// -----------------------------------------------------------------------------
// Frontend setup. Call once from any thread (typically main) before logging.
// Creates the sink(s) and the root logger and acquires the manual backend worker.
// This does NOT bind the backend to a thread — see start_logger().
LEPTON_API void init_logger(const LoggerConfig& config = {});

// Returns true once init_logger() has successfully created the root logger.
LEPTON_API bool is_initialized() noexcept;

// Change the minimum level at runtime. Safe to call from any thread.
LEPTON_API void set_log_level(LogLevel level) noexcept;

// Backend lifecycle. lepton does NOT own a polling thread — User drives one, so a
// single worker thread can poll lepton alongside your own logger.
// start_logger(), the poll_logger_* calls and stop_logger() MUST all run on that SAME thread
//   - start_logger() binds the backend worker to the calling thread.
//   - poll_logger_*() drain queued records; call repeatedly in your loop.
//   - stop_logger() flushes the remaining records and releases the worker.
// Prefer the PollLoggerScope RAII helper below over calling start/stop by hand.
LEPTON_API void start_logger();
LEPTON_API void stop_logger();

// Two poll styles — pick by how you share the polling thread:
//   - poll_logger_once():   process at most ONE record, then return. Lowest,
//                           most predictable per-call cost;
//   - poll_logger_for(us):  drain until empty OR until `us` microseconds elapse,
//                           whichever comes first(Bounded latency)
LEPTON_API void poll_logger_once();
LEPTON_API void poll_logger_for(unsigned timeout_us);

// PollLoggerScope: RAII binding of the backend worker to the CURRENT thread. Binds on
// construction, drains + releases on destruction — both on this thread. 
// This is the intended way to run the poll loop:
//
//   void my_logging_thread() {
//       lepton::PollLoggerScope scope;           // start_logger() here
//       while (running) {
//           lepton::poll_logger_for(50);         // drain lepton logs
//           fmtlog::poll();                      // drain the host's own logs
//       }
//   }  // <- stop_logger() (drain + shutdown) here, same thread
class PollLoggerScope {
public:
    PollLoggerScope() noexcept { start_logger(); }
    ~PollLoggerScope() noexcept { stop_logger(); }

    PollLoggerScope(PollLoggerScope const&) = delete;
    PollLoggerScope& operator=(PollLoggerScope const&) = delete;
    PollLoggerScope(PollLoggerScope&&) = delete;
    PollLoggerScope& operator=(PollLoggerScope&&) = delete;
};

namespace fmt = ::fmtquill;

} // namespace lepton

// -----------------------------------------------------------------------------
// Logging Macros
// -----------------------------------------------------------------------------
#define LEPTON_LOG_DEBUG(fmt_str, ...)                                           \
    do {                                                                         \
        LEPTON_ASSERT_LOGGER_READY();                                            \
        QUILL_LOG_DEBUG(::lepton::lepton_root_logger, fmt_str, ##__VA_ARGS__);   \
    } while (0)

#define LEPTON_LOG_INFO(fmt_str, ...)                                            \
    do {                                                                         \
        LEPTON_ASSERT_LOGGER_READY();                                            \
        QUILL_LOG_INFO(::lepton::lepton_root_logger, fmt_str, ##__VA_ARGS__);    \
    } while (0)

#define LEPTON_LOG_WARN(fmt_str, ...)                                            \
    do {                                                                         \
        LEPTON_ASSERT_LOGGER_READY();                                            \
        QUILL_LOG_WARNING(::lepton::lepton_root_logger, fmt_str, ##__VA_ARGS__); \
    } while (0)

#define LEPTON_LOG_ERROR(fmt_str, ...)                                           \
    do {                                                                         \
        LEPTON_ASSERT_LOGGER_READY();                                            \
        QUILL_LOG_ERROR(::lepton::lepton_root_logger, fmt_str, ##__VA_ARGS__);   \
    } while (0)
