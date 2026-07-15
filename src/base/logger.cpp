#include "lepton/base/logger.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/FileSink.h"

namespace lepton {

LEPTON_API quill::Logger* lepton_root_logger = nullptr;
static quill::ManualBackendWorker* lepton_manual_worker = nullptr;

LEPTON_API void init_logger(quill::LogLevel log_level, bool use_console_logger) noexcept {
    if (lepton_root_logger != nullptr) return;

    lepton_manual_worker = quill::Backend::acquire_manual_backend_worker();

    quill::BackendOptions options;
    lepton_manual_worker->init(options);

    if (use_console_logger) {
        auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("lepton_console");
        lepton_root_logger = quill::Frontend::create_or_get_logger("lepton", std::move(console_sink));
    } else {
        // Use daily rolling file sink to protect performance limits
        auto file_sink = quill::Frontend::create_or_get_sink<quill::FileSink>(
            "lepton.log",
            []() {
                quill::FileSinkConfig cfg;
                cfg.set_open_mode('w');
                cfg.set_filename_append_option(quill::FilenameAppendOption::StartDateTime);
                return cfg;
            }(),
            quill::FileEventNotifier{});
        lepton_root_logger = quill::Frontend::create_or_get_logger(
            "lepton_core", std::move(file_sink),
            quill::PatternFormatterOptions{
                "%(time) [%(thread_id)] %(short_source_location:<20) "
                "[%(log_level:<5)] %(message)",
                "%H:%M:%S.%Qns", quill::Timezone::GmtTime});
    }

    lepton_root_logger->set_log_level(log_level);
}

LEPTON_API void poll_logger() {
    if (lepton_manual_worker) {
        // Processes exactly one log record from the queue per call
        // Perfect for embedding in a busy loop without causing timing jitter
        lepton_manual_worker->poll_one();
    }
}

} // namespace lepton
