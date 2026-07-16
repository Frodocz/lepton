include(FetchContent)

# -----------------------------------------------------------------------------
# Third-party logging libraries used only by the integration examples.
# These are NOT dependencies of the lepton library itself -- they are here purely
# to demonstrate that a host application's own logger can coexist with lepton.
# -----------------------------------------------------------------------------
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.0
)
FetchContent_MakeAvailable(spdlog)

# fmtlog is header-based (fmtlog-inl.h compiled once). Fetch it and expose an
# INTERFACE-ish static target that carries the include dir plus the single TU.
FetchContent_Declare(
    fmtlog
    GIT_REPOSITORY https://github.com/MengRao/fmtlog.git
    GIT_TAG        v2.3.0
)
FetchContent_GetProperties(fmtlog)
if(NOT fmtlog_POPULATED)
    FetchContent_Populate(fmtlog)
endif()

# fmtlog is one header (fmtlog.h) + one TU (fmtlog.cc -> fmtlog-inl.h), and it
# bundles its own copy of {fmt} under fmt/include. Compile the TU into a static
# lib and expose both the fmtlog dir and its bundled fmt include dir.
add_library(fmtlog STATIC ${fmtlog_SOURCE_DIR}/fmtlog.cc)
target_include_directories(fmtlog BEFORE PUBLIC
    ${fmtlog_SOURCE_DIR}
    ${fmtlog_SOURCE_DIR}/fmt/include)
# Use fmt in header-only mode so we don't need to also build fmt as a library.
target_compile_definitions(fmtlog PUBLIC FMT_HEADER_ONLY)

# -----------------------------------------------------------------------------
# Example targets
# -----------------------------------------------------------------------------
macro(lepton_add_example EX_NAME)
    add_executable(${EX_NAME} ${ARGN})
    target_compile_features(${EX_NAME} PRIVATE cxx_std_20)
    target_compile_options(${EX_NAME} PRIVATE -O3 -march=native)
    target_link_libraries(${EX_NAME} PRIVATE lepton::lepton)
    message(STATUS "Registered Example Target: ${EX_NAME}")
endmacro()

# -----------------------------------------------------------------------------
# Examples to handle Lepton's logger module with your project's logger module
# -----------------------------------------------------------------------------
# Long-running server config: multi-sink + rotating file + compile-time cut.
lepton_add_example(server_logging server_logging.cpp)

# Host also uses Quill directly, shared single backend, one poll loop.
lepton_add_example(shared_quill_logger shared_quill_logger.cpp)

# Host uses fmtlog; one thread polls both fmtlog and lepton.
lepton_add_example(with_fmtlog with_fmtlog_logging.cpp)
target_include_directories(with_fmtlog BEFORE PRIVATE 
    ${fmtlog_SOURCE_DIR}
    ${fmtlog_SOURCE_DIR}/fmt/include)
target_link_libraries(with_fmtlog PRIVATE fmtlog)

# Host uses spdlog(async); spdlog's pool thread and lepton's poll thread coexist.
lepton_add_example(with_spdlog with_spdlog_logging.cpp)
target_link_libraries(with_spdlog PRIVATE spdlog::spdlog)
