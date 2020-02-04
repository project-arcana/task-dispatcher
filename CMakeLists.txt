cmake_minimum_required(VERSION 3.8)
project(task-dispatcher)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(cmake/UnityBuild.cmake)

# =========================================
# options

option(TD_ALLOW_HEAP_TASKS "If enabled, automatically create heap-allocated tasks for lambdas exceeding cacheline size" OFF)
option(TD_ENABLE_UNITY_BUILD "If enabled, compiles this library as a single compilation unit" ON)

# =========================================
# define library

file(GLOB_RECURSE SOURCES "src/*.cc")
file(GLOB_RECURSE HEADERS "src/*.hh")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${HEADERS})

if (TD_ENABLE_UNITY_BUILD)
    enable_unity_build(task-dispatcher SOURCES 50 cc)
endif()

add_library(task-dispatcher STATIC ${SOURCES} ${HEADERS})
target_include_directories(task-dispatcher PUBLIC src/)
target_link_libraries(task-dispatcher PUBLIC clean-core)

if (TD_ALLOW_HEAP_TASKS)
    target_compile_definitions(task-dispatcher PUBLIC TD_ALLOW_HEAP_TASKS)
endif()

# =========================================
# set up compile flags

# default to RelWithDebInfo
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
        "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()

if (MSVC)
    target_compile_options(task-dispatcher PUBLIC /MP /GT)
else()
    # Pthread
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(task-dispatcher PRIVATE Threads::Threads)

    target_compile_options(task-dispatcher PRIVATE -Wall -fPIC)
    target_link_libraries(task-dispatcher PUBLIC -fuse-ld=gold)
endif()
