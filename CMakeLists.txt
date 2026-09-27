cmake_minimum_required(VERSION 3.20)
project(devops-sandbox VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(sandbox
    src/main.cpp
    src/redis_client.cpp
    src/sandbox.cpp
    src/seccomp.cpp
    src/metrics.cpp
    src/health_monitor.cpp
    src/log_aggregator.cpp
    src/resource_monitor.cpp
    src/process_supervisor.cpp
)

target_include_directories(sandbox PRIVATE include)

if(APPLE)
    target_link_libraries(sandbox PRIVATE pthread)
else()
    target_link_libraries(sandbox PRIVATE pthread rt seccomp)
endif()

enable_testing()

add_executable(sandbox_tests
    tests/test_main.cpp
    src/metrics.cpp
    src/log_aggregator.cpp)

target_include_directories(sandbox_tests PRIVATE include tests)

add_test(NAME sandbox_unit_tests COMMAND sandbox_tests)
