add_executable(kickmsg_unit
    unit/types-t.cc
    unit/region-t.cc
    unit/publisher-t.cc
    unit/subscriber-t.cc
    unit/wait_fd-t.cc
    unit/wait_any-t.cc
    unit/naming-t.cc
    unit/node-t.cc
    unit/registry-t.cc
    unit/blackboard-t.cc
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
    stress/big_payload.cc
    stress/blackboard.cc
    stress/wake_carrier.cc
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

# Crash test uses fork/waitpid/SIGKILL -- POSIX only
if(NOT WIN32)
    add_executable(kickmsg_crash_test crash_test.cc)
    target_link_libraries(kickmsg_crash_test PRIVATE kickmsg)
    set_target_properties(kickmsg_crash_test
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME crash COMMAND kickmsg_crash_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(crash PROPERTIES TIMEOUT 300)

    # Stall/repair test SIGSTOPs a forked publisher and steals its stale
    # entry locks from the parent -- POSIX only, like the crash test.
    add_executable(kickmsg_stall_repair_test stall_repair_test.cc)
    target_link_libraries(kickmsg_stall_repair_test PRIVATE kickmsg)
    set_target_properties(kickmsg_stall_repair_test
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME stall_repair COMMAND kickmsg_stall_repair_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(stall_repair PROPERTIES TIMEOUT 300)

    # Multi-process steady-state MPMC: forked publishers/subscribers over a
    # parent-created region, exact conservation oracles -- POSIX only.
    add_executable(kickmsg_mp_stress_test mp_stress_test.cc)
    target_link_libraries(kickmsg_mp_stress_test PRIVATE kickmsg)
    set_target_properties(kickmsg_mp_stress_test
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME mp_stress COMMAND kickmsg_mp_stress_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(mp_stress PROPERTIES TIMEOUT 300)

    # Blackboard crash recovery: fork a key owner, SIGKILL it mid-update,
    # verify the value survives, the entry is sweepable, and a restarted
    # owner takes the key over -- POSIX only, like the other fork tests.
    add_executable(kickmsg_blackboard_crash_test blackboard_crash_test.cc)
    target_link_libraries(kickmsg_blackboard_crash_test PRIVATE kickmsg)
    set_target_properties(kickmsg_blackboard_crash_test
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME blackboard_crash COMMAND kickmsg_blackboard_crash_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(blackboard_crash PROPERTIES TIMEOUT 300)

    add_executable(kickmsg_registry_stress_test registry_stress_test.cc)
    target_link_libraries(kickmsg_registry_stress_test PRIVATE kickmsg)
    set_target_properties(kickmsg_registry_stress_test
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME registry_stress COMMAND kickmsg_registry_stress_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(registry_stress PROPERTIES TIMEOUT 300)
endif()
