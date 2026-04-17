add_executable(kickmsg_unit
    unit/types-t.cc
    unit/region-t.cc
    unit/publisher-t.cc
    unit/subscriber-t.cc
    unit/naming-t.cc
    unit/node-t.cc
    unit/registry-t.cc
)
target_link_libraries(kickmsg_unit PRIVATE kickmsg GTest::gmock_main)
set_target_properties(kickmsg_unit
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME kickmsg COMMAND kickmsg_unit WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(kickmsg_stress_test
    stress/main.cc
    stress/treiber.cc
    stress/churn.cc
    stress/gc_recovery.cc
    stress/fairness.cc
    stress/mpmc.cc
    stress/pool_exhaustion.cc
    stress/live_repair.cc
    stress/edge_cases.cc
)
target_link_libraries(kickmsg_stress_test PRIVATE kickmsg)
set_target_properties(kickmsg_stress_test
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Crash test uses fork/waitpid/SIGKILL — POSIX only
if(NOT WIN32)
    add_executable(kickmsg_crash_test crash_test.cc)
    target_link_libraries(kickmsg_crash_test PRIVATE kickmsg)
    set_target_properties(kickmsg_crash_test
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
