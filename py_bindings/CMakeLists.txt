if (SKBUILD)
    set(LIB_SRCS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/kickmsg_py.cc
        )

    find_package(Python
        REQUIRED COMPONENTS Interpreter Development.Module
        OPTIONAL_COMPONENTS Development.SABIModule)
    execute_process(
        COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_ROOT)
    find_package(nanobind CONFIG REQUIRED)

    nanobind_add_module(kickmsg_py ${LIB_SRCS} NB_STATIC STABLE_ABI)
    target_link_libraries(kickmsg_py PRIVATE kickmsg)
    target_compile_options(kickmsg_py PRIVATE -Wno-shadow -Wno-cast-qual -Wno-pedantic)

    # GNU-ld-only size optimizations.  ld64 (Apple) silently ignores some
    # -Wl flags and errors on others; MSVC link is entirely different.
    if(NOT APPLE AND NOT MSVC)
        target_link_options(kickmsg_py PRIVATE
            $<$<CONFIG:Release>:-Wl,--gc-sections -Wl,--strip-all>)
    endif()

    # Output name is 'kickmsg' so Python imports as `import kickmsg`.
    set_target_properties(kickmsg_py PROPERTIES OUTPUT_NAME "kickmsg")
    install(TARGETS kickmsg_py DESTINATION .)
endif()
