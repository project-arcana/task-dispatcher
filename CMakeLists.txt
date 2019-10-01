cmake_minimum_required(VERSION 3.8)
project(task-dispatcher)


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =========================================
# global options

option(TD_STRICT "if true, enables all warnings and -Werror" OFF)

# =========================================
# define library

file(GLOB_RECURSE SOURCES "src/*.cc" "src/*.hh")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${SOURCES})

add_executable(task-dispatcher ${SOURCES})
target_include_directories(task-dispatcher PUBLIC src/)

# =========================================
# set up compile flags

# default to RelWithDebInfo
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
        "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()

if (MSVC)
    target_compile_options(task-dispatcher PUBLIC /MP)
else()
    target_compile_options(task-dispatcher PRIVATE -Wall -fPIC)
    target_link_libraries(task-dispatcher PUBLIC -fuse-ld=gold)
endif()

target_compile_definitions(task-dispatcher PUBLIC $<$<CONFIG:DEBUG>:CC_DEBUG>)
target_compile_definitions(task-dispatcher PUBLIC $<$<CONFIG:RELEASE>:CC_RELEASE>)
target_compile_definitions(task-dispatcher PUBLIC $<$<CONFIG:RELWITHDEBINFO>:CC_RELWITHDEBINFO>)

if (CC_STRICT)
    if (MSVC)
        target_compile_definitions(task-dispatcher PRIVATE /WX /W4)
    else()
        target_compile_options(task-dispatcher PRIVATE -Wextra -Wpedantic -Werror)
    endif()
endif()
