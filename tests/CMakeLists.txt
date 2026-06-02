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
    stress/common.cc
    stress/treiber.cc
    stress/churn.cc
    stress/gc_recovery.cc
    stress/fairness.cc
    stress/mpmc.cc
    stress/pool_exhaustion.cc
    stress/live_repair.cc
    stress/edge_cases.cc
)
target_link_libraries(kickmsg_stress_test PRIVATE kickmsg argparse::argparse)
set_target_properties(kickmsg_stress_test
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
# Contention thread counts scale to the host core count (see contention_count
# / the --oversub flag), keeping the suite bounded on 2-4 core CI runners
# instead of the oversubscription livelock a fixed 32-thread count caused.
# Generous timeout to absorb slow or TSAN-instrumented runs.
# Follow-up: split this integration/stress suite from the unit tests so it can
# run as its own CI job rather than alongside kickmsg_unit.
add_test(NAME stress COMMAND kickmsg_stress_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(stress PROPERTIES TIMEOUT 300)

# Crash test uses fork/waitpid/SIGKILL — POSIX only
if(NOT WIN32)
    add_executable(kickmsg_crash_test crash_test.cc)
    target_link_libraries(kickmsg_crash_test PRIVATE kickmsg)
    set_target_properties(kickmsg_crash_test
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME crash COMMAND kickmsg_crash_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(crash PROPERTIES TIMEOUT 300)

    add_executable(kickmsg_registry_stress_test registry_stress_test.cc)
    target_link_libraries(kickmsg_registry_stress_test PRIVATE kickmsg)
    set_target_properties(kickmsg_registry_stress_test
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME registry_stress COMMAND kickmsg_registry_stress_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(registry_stress PROPERTIES TIMEOUT 300)
endif()
