cmake_minimum_required(VERSION 3.12...4.0)

project(lepton CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

#-------------------------------------------------------------------------------------------------------
# Options
#-------------------------------------------------------------------------------------------------------
option(LEPTON_BUILD_EXAMPLES "Build example programs" OFF)
option(LEPTON_BUILD_MODULE "Build the experimental C++20 named module target." OFF)
option(LEPTON_BUILD_TESTS "Build unit tests" ON)
option(LEPTON_ENABLE_LOGGER "Enable logs for the framework" ON)
option(LEPTON_NO_EXCEPTIONS "Don't throw exceptions" OFF)
option(LEPTON_USE_FSTACK "Use F-Stack + DPDK as the network backend" OFF)

#-------------------------------------------------------------------------------------------------------
# Set default build to release
#-------------------------------------------------------------------------------------------------------
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build" FORCE)
endif ()
message(STATUS "CMAKE_BUILD_TYPE: " ${CMAKE_BUILD_TYPE})

message(STATUS "LEPTON_BUILD_EXAMPLES: " ${LEPTON_BUILD_EXAMPLES})
message(STATUS "LEPTON_BUILD_TESTS: " ${LEPTON_BUILD_TESTS})
message(STATUS "LEPTON_ENABLE_LOGGER: " ${LEPTON_ENABLE_LOGGER})
message(STATUS "LEPTON_USE_FSTACK: " ${LEPTON_USE_FSTACK})
message(STATUS "LEPTON_NO_EXCEPTIONS: " ${LEPTON_NO_EXCEPTIONS})

# -----------------------------------------------------------------------------
# Dependency Management
# -----------------------------------------------------------------------------
include(FetchContent)

# find_package(absl CONFIG)
# if (NOT absl_FOUND)
#     message(STATUS "Abseil not found locally. Fetch from GitHub...")
#     FetchContent_Declare(
#         absl
#         GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
#         GIT_TAG        20260526.0
#     )
#     FetchContent_MakeAvailable(absl)
# endif()

FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt
    GIT_TAG        12.2.0
)

FetchContent_Declare(
    quill
    GIT_REPOSITORY https://github.com/odygrd/quill.git
    GIT_TAG        v12.0.0
)

FetchContent_MakeAvailable(fmt quill)

# -----------------------------------------------------------------------------
# Source & Header Files
# -----------------------------------------------------------------------------
set(LEPTON_HEADERS
    include/lepton/base/attributes.h
    include/lepton/base/buffer_pool.h
    include/lepton/base/io_buffer.h
    include/lepton/base/lepton_error.h
    include/lepton/base/logger.h

    include/lepton/net/endpoint.h
    include/lepton/net/stream.h
    include/lepton/net/sys_api.h
    include/lepton/net/tcp_socket.h
)

set(LEPTON_SOURCES
    src/base/logger.cpp

    src/net/endpoint.cpp
)

# -----------------------------------------------------------------------------
# Target Definition (Dynamically handles .a or .so)
# -----------------------------------------------------------------------------
add_library(lepton
    ${LEPTON_HEADERS}
    ${LEPTON_SOURCES}
)

add_library(lepton::lepton ALIAS lepton)

# -----------------------------------------------------------------------------
# Target Includes & Compile Options
# -----------------------------------------------------------------------------
target_include_directories(lepton
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(lepton 
    PRIVATE 
    -O3 
    -march=native 
    -Wall
    -Wextra
)

find_package(OpenSSL REQUIRED)
target_link_libraries(
    lepton
    PUBLIC
    # OpenSSL::SSL
    # OpenSSL::Crypto
    fmt::fmt
    quill::quill

    # absl::flat_hash_map
    # absl::strings
)

target_compile_options(lepton
    PUBLIC
    -Wno-deprecated-declarations
)

# Check if the compiler supports AVX2
include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("-mavx2" COMPILER_SUPPORTS_AVX2)

if(COMPILER_SUPPORTS_AVX2 AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64"))
    message(STATUS "AVX2 supported. Adding -mavx2 flag.")
    target_compile_options(lepton
        PUBLIC
        -mavx2
    )
else()
    message(STATUS "AVX2 not supported by the compiler.")
endif()

if(LEPTON_USE_FSTACK)
    target_compile_definitions(lepton PUBLIC -DLEPTON_USE_FSTACK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)
    target_compile_options(lepton PRIVATE ${DPDK_CFLAGS})
    # These must be PUBLIC: lepton is a static library, so the ff_*/DPDK symbols
    # it references are only resolved on the FINAL link of each consumer
    # (examples, tests, your binary). PUBLIC propagates them there, in
    # the right order (after liblepton.a). --whole-archive is required because
    # F-Stack registers DPDK PMDs via constructors that would otherwise be
    # dropped as "unreferenced".
    # F-Stack install prefix for libfstack.a (override with -DFSTACK_LIB_DIR=...).
    if(NOT DEFINED FSTACK_LIB_DIR)
        set(FSTACK_LIB_DIR /usr/local/lib)
    endif()
    # F-Stack's static archive pulls in FreeBSD-derived subsystems (netgraph
    # deflate -> zlib, IPsec -> crypto/ssl, bpf -> pcap). List them AFTER
    # -lfstack so the linker can resolve the references.
    target_link_options(lepton
        PUBLIC
        -L${FSTACK_LIB_DIR}
        ${DPDK_STATIC_LDFLAGS}
        "SHELL:-Wl,--whole-archive,-lfstack,--no-whole-archive"
        -pthread
        -lm
        -lrt
        -ldl
        -lnuma
        -lz
        -lpcap
        -lcrypto
        -lssl
    )
endif()

if(LEPTON_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    enable_testing()
    add_subdirectory(tests)
endif()

if(LEPTON_BUILD_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples)
    add_subdirectory(examples)
endif()
