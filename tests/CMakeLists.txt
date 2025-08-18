cmake_minimum_required(VERSION 3.11)

file(GLOB_RECURSE SOURCES
    "*.cc"
    "*.hh"
)

add_arcana_test(tests-task-dispatcher "${SOURCES}")

target_link_libraries(tests-task-dispatcher PUBLIC
    clean-core
    task-dispatcher
)
