cmake_minimum_required(VERSION 3.8)
project(task-dispatcher)

if (NOT TARGET clean-core)
    message(FATAL_ERROR "[task-dispatcher] clean-core must be available")
endif()

# =========================================
# options

option(TD_ALLOW_HEAP_TASKS "If enabled, automatically create heap-allocated tasks for lambdas exceeding cacheline size" OFF)
option(TD_NO_WAITS "If enabled, worker threads do not wait/sleep when idle, minimizing latency and maximizing throughput at the cost of CPU usage" OFF)

# =========================================
# define library

file(GLOB_RECURSE SOURCES "src/*.cc")
file(GLOB_RECURSE HEADERS "src/*.hh")

arcana_add_library(TD task-dispatcher SOURCES HEADERS)

target_include_directories(task-dispatcher PUBLIC src/)

target_link_libraries(task-dispatcher PUBLIC
    clean-core
)

if (TD_ALLOW_HEAP_TASKS)
    target_compile_definitions(task-dispatcher PUBLIC TD_ALLOW_HEAP_TASKS)
endif()

if (TD_NO_WAITS)
    target_compile_definitions(task-dispatcher PUBLIC TD_NO_WAITS)
endif()

# =========================================
# set up compile flags

if (MSVC)
    # /GT: fiber-safe optimizations
    target_compile_options(task-dispatcher PUBLIC /GT)

    # Winmm.lib for timeapi.h usage (timeBeginPeriod)
    target_link_libraries(task-dispatcher PRIVATE "Winmm.lib")
else()
    # Pthread
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(task-dispatcher PRIVATE Threads::Threads)
endif()
