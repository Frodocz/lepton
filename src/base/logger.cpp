#include "lepton/base/logger.h"
#include "lepton/base/lepton_error.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/RotatingFileSink.h"

#include <memory>
#include <vector>

namespace lepton {

LEPTON_API quill::Logger* lepton_root_logger = nullptr;
static quill::ManualBackendWorker* lepton_manual_worker = nullptr;

// The pattern shared by all lepton sinks.
static constexpr char kPatternFormat[] =
    "%(time) [%(thread_id)] %(short_source_location:<20) [%(log_level:<5)] %(message)";

static quill::LogLevel to_quill_level(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug:    return quill::LogLevel::Debug;
        case LogLevel::Info:     return quill::LogLevel::Info;
        case LogLevel::Warning:  return quill::LogLevel::Warning;
        case LogLevel::Error:    return quill::LogLevel::Error;
    }
    return quill::LogLevel::Info;
}

LEPTON_API void init_logger(const LoggerConfig& config) {
    if (lepton_root_logger != nullptr) return;

    // Acquire the manual backend worker but do NOT init() it here: init()
    // stamps the calling thread as the backend thread, and poll/shutdown must
    // run on that same thread. Binding happens in start_logger() instead.
    lepton_manual_worker = quill::Backend::acquire_manual_backend_worker();

    quill::PatternFormatterOptions pattern{
        config.pattern.empty() ? kPatternFormat : config.pattern.c_str(),
        "%H:%M:%S.%Qns", quill::Timezone::GmtTime};
    
    std::vector<std::shared_ptr<quill::Sink>> sinks;
    if (config.to_console) {
        sinks.push_back(
            quill::Frontend::create_or_get_sink<quill::ConsoleSink>("lepton_console"));
    }

    if (config.to_rotating_file) {
        // Rotating file sink: bounds disk usage for a long-running process.
        sinks.push_back(quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(
            config.filename,
            [&config]() {
                quill::RotatingFileSinkConfig cfg;
                cfg.set_open_mode('a');
                cfg.set_filename_append_option(quill::FilenameAppendOption::StartDateTime);
                if (config.max_file_size_bytes > 0) {
                    cfg.set_rotation_max_file_size(config.max_file_size_bytes);
                }
                cfg.set_max_backup_files(config.max_backup_files);
                cfg.set_overwrite_rolled_files(true);
                if (!config.daily_rotation_time.empty()) {
                    cfg.set_rotation_time_daily(config.daily_rotation_time);
                }
                return cfg;
            }()));
    }

    if (sinks.empty()) {
        LEPTON_THROW(LeptonError{"logger init: no sink enabled"});
        return;
    }

    lepton_root_logger =
        quill::Frontend::create_or_get_logger("lepton", std::move(sinks), pattern);
    lepton_root_logger->set_log_level(to_quill_level(config.level));
}

LEPTON_API void poll_logger() {
    if (lepton_manual_worker) {
        // Processes exactly one log record from the queue per call
        // Perfect for embedding in a busy loop without causing timing jitter
        lepton_manual_worker->poll_one();
    }
}

LEPTON_API bool is_initialized() noexcept {
    return lepton_root_logger != nullptr;
}

LEPTON_API void set_log_level(LogLevel level) noexcept {
    if (lepton_root_logger) {
        lepton_root_logger->set_log_level(to_quill_level(level));
    }
}

LEPTON_API void start_logger() {
    if (lepton_manual_worker) {
        // Binds the backend worker to THIS thread. Every subsequent poll and the
        // eventual stop_logger() must be called from this same thread.
        quill::BackendOptions options;
        lepton_manual_worker->init(options);
    }
}

LEPTON_API void poll_logger_once() {
    if (lepton_manual_worker) {
        // Processes at most one log record per call: predictable, low per-call
        // cost, ideal when folded into a busy loop that also does other work.
        lepton_manual_worker->poll_one();
    }
}

LEPTON_API void poll_logger_for(unsigned timeout_us) {
    if (lepton_manual_worker) {
        // Drains until empty or until the timeout elapses, whichever comes first:
        // bounded latency, the right fit for a thread shared with another logger.
        lepton_manual_worker->poll(std::chrono::microseconds{timeout_us});
    }
}

LEPTON_API void stop_logger() {
    if (lepton_manual_worker) {
        // Drain everything still queued, then release the worker. Must run on the
        // same thread that called start_logger().
        lepton_manual_worker->poll();
        lepton_manual_worker->shutdown();
    }
}

} // namespace lepton
