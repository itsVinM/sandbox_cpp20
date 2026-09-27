cmake_minimum_required(VERSION 3.20)
project(devops-sandbox VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(sandbox
    devops/main.cpp
    devops/redis_client.cpp
    devops/sandbox.cpp
    devops/seccomp.cpp
    devops/metrics.cpp
    devops/health_monitor.cpp
    devops/log_aggregator.cpp
    devops/resource_monitor.cpp
    devops/process_supervisor.cpp
)

target_include_directories(sandbox PRIVATE .)

if(APPLE)
    target_link_libraries(sandbox PRIVATE pthread)
else()
    target_link_libraries(sandbox PRIVATE pthread rt seccomp)
endif()

enable_testing()

add_executable(sandbox_tests
    tests/test_main.cpp
    devops/metrics.cpp
    devops/log_aggregator.cpp)

target_include_directories(sandbox_tests PRIVATE . tests)

add_test(NAME sandbox_unit_tests COMMAND sandbox_tests)
