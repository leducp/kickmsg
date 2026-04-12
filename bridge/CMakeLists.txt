add_library(kickmsg_bridge STATIC
    src/Protocol.cc
)

target_include_directories(kickmsg_bridge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(kickmsg_bridge PUBLIC kickmsg)

if(BUILD_UNIT_TESTS)
    add_executable(kickmsg_bridge_unit
        tests/protocol-t.cc
    )
    target_link_libraries(kickmsg_bridge_unit PRIVATE kickmsg_bridge GTest::gmock_main)
    set_target_properties(kickmsg_bridge_unit
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME kickmsg_bridge COMMAND kickmsg_bridge_unit WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
