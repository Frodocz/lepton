#pragma once

/// @file init.h
/// @brief Lepton framework global environment initialization.

#include "lepton/base/logger.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

#if defined(LEPTON_USE_FSTACK)
#include <ff_api.h>
#endif

namespace lepton {

/// Initialize the Lepton framework and underlying network/system stack (e.g. F-Stack/DPDK).
/// Guarantee thread-safe, idempotent initialization (safe to call multiple times).
///
/// @param argc Command line argument count
/// @param argv Command line argument vector
/// @param app_name Application identifier
/// @return 0 on success, negative error code on failure
inline int init([[maybe_unused]] int argc, [[maybe_unused]] char** argv, [[maybe_unused]] const char* app_name = "lepton_app") {
    static std::atomic<bool> s_initialized{false};
    if (s_initialized.load(std::memory_order_acquire)) {
        return 0; // Already initialized in this process
    }

#if defined(LEPTON_USE_FSTACK)
    static std::mutex s_init_mutex;
    std::lock_guard<std::mutex> lock(s_init_mutex);
    if (s_initialized.load(std::memory_order_relaxed)) {
        return 0;
    }

    bool has_conf = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && std::strcmp(argv[i], "--conf") == 0) {
            has_conf = true;
            break;
        }
    }

    int ret = 0;
    if (has_conf) {
        ret = ff_init(argc, argv);
        if (ret < 0) {
            LEPTON_LOG_ERROR("F-Stack ff_init failed with provided arguments!");
        }
    } else {
        const char* candidate_paths[] = {
            "examples/f-stack.config.example.ini",
            "../examples/f-stack.config.example.ini",
            "../../examples/f-stack.config.example.ini"
        };
        std::string found_path;
        for (const char* path : candidate_paths) {
            if (std::filesystem::exists(path)) {
                found_path = path;
                break;
            }
        }
        if (found_path.empty()) {
            found_path = "examples/f-stack.config.example.ini";
        }

        std::string name_arg = lepton::fmt::format("{}", app_name);
        std::string conf_arg = lepton::fmt::format("{}", found_path);
        char arg1[] = "--conf";
        char* fstack_argv[] = { name_arg.data(), arg1, conf_arg.data(), nullptr };
        ret = ff_init(3, fstack_argv);
        if (ret < 0) {
            LEPTON_LOG_ERROR("F-Stack ff_init failed with config file: {}", conf_arg);
        }
    }

    if (ret < 0) {
        return ret;
    }

    s_initialized.store(true, std::memory_order_release);
    return 0;
#else
    s_initialized.store(true, std::memory_order_release);
    return 0;
#endif
}

} // namespace lepton
