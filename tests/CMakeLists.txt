set(CMAKE_CXX_STANDARD 20)

include(CTest)

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
)
FetchContent_GetProperties(googletest)
if(NOT googletest_POPULATED)
    FetchContent_Populate(googletest)
    add_subdirectory(${googletest_SOURCE_DIR} ${googletest_BINARY_DIR})
endif()

add_library(lepton_test_main STATIC test_main.cpp)
target_compile_features(lepton_test_main PRIVATE cxx_std_20)
target_link_libraries(lepton_test_main PUBLIC
    lepton::lepton
    GTest::gtest
)

macro(lepton_add_test TEST_NAME)
    # Define the executable target using the provided name and trailing source arguments
    add_executable(${TEST_NAME} ${ARGN})

    # Configure target options matching C++20 parameters
    target_compile_features(${TEST_NAME} PRIVATE cxx_std_20)

    target_link_libraries(${TEST_NAME} PRIVATE
        lepton_test_main
    )

    # Mirror matching optimization flags for consistent testing environments
    target_compile_options(${TEST_NAME} PRIVATE -O3 -march=native)

    # Register the binary into the CTest harness architecture
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

    message(STATUS "Registered Test Target: ${TEST_NAME}")
endmacro()

# test_base_logger has its own main()
add_executable(test_base_logger test_base_logger.cpp)
target_compile_features(test_base_logger PRIVATE cxx_std_20)
target_link_libraries(test_base_logger PRIVATE lepton::lepton)
target_compile_options(test_base_logger PRIVATE -O3 -march=native)
add_test(NAME test_base_logger COMMAND test_base_logger)
message(STATUS "Registered Test Target: test_base_logger")

lepton_add_test(test_buffer_pool test_buffer_pool.cpp)
lepton_add_test(test_tcp_socket test_tcp_socket.cpp)
lepton_add_test(test_ws test_ws.cpp)
lepton_add_test(test_security test_security.cpp)
lepton_add_test(test_http test_http.cpp)
lepton_add_test(test_ws_session test_ws_session.cpp)
lepton_add_test(test_http_client test_http_client.cpp)