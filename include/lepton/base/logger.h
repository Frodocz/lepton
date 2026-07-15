#pragma once

#define QUILL_DISABLE_NON_PREFIXED_MACROS

#include "lepton/base/attributes.h"

#include "quill/LogMacros.h"
#include "quill/Logger.h"

namespace lepton {

extern LEPTON_API quill::Logger* lepton_root_logger;


// Lifecycle management functions
LEPTON_API void init_logger(quill::LogLevel log_level = quill::LogLevel::Debug,
                            bool use_console_logger = true) noexcept;
LEPTON_API void poll_logger();

} // namespace lepton

// -----------------------------------------------------------------------------
// Logging Macros
// -----------------------------------------------------------------------------
#define LEPTON_LOG_DEBUG(fmt_str, ...) \
    QUILL_LOG_DEBUG(::lepton::lepton_root_logger, fmt_str, ##__VA_ARGS__)

#define LEPTON_LOG_INFO(fmt_str, ...)  \
    QUILL_LOG_INFO(::lepton::lepton_root_logger, fmt_str, ##__VA_ARGS__)

#define LEPTON_LOG_WARN(fmt_str, ...)  \
    QUILL_LOG_WARNING(::lepton::lepton_root_logger, fmt_str, ##__VA_ARGS__)

#define LEPTON_LOG_ERROR(fmt_str, ...) \
    QUILL_LOG_ERROR(::lepton::lepton_root_logger, fmt_str, ##__VA_ARGS__)
