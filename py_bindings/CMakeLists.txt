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

    # The package layout is:
    #   kickmsg/
    #     __init__.py      re-exports _native + surfaces diagnostics
    #     _native.*.so     this target
    #     _native.pyi      type stubs for the native surface
    #     diagnostics.py   pure-Python typed API (importable by GUIs)
    #     _cli.py          argparse entry point (`kickmsg` console_script)
    #     py.typed         PEP 561 marker
    set_target_properties(kickmsg_py PROPERTIES
        OUTPUT_NAME "_native"
        # Drop the native module into build_py/py_bindings/kickmsg/ so
        # `PYTHONPATH=build_py/py_bindings` gives a valid package without a
        # full wheel install.
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/kickmsg")

    # Stage the Python sources alongside the native module for the same
    # PYTHONPATH-pointed dev flow.  Symlinks (not copies) so edits to the
    # .py sources are picked up on the next invocation without a rebuild.
    set(PY_PACKAGE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/python/kickmsg)
    set(PY_PACKAGE_DST ${CMAKE_CURRENT_BINARY_DIR}/kickmsg)
    file(GLOB PY_PACKAGE_FILES
        "${PY_PACKAGE_SRC}/*.py"
        "${PY_PACKAGE_SRC}/*.pyi"
        "${PY_PACKAGE_SRC}/py.typed")
    foreach(_src ${PY_PACKAGE_FILES})
        get_filename_component(_name ${_src} NAME)
        file(CREATE_LINK ${_src} ${PY_PACKAGE_DST}/${_name}
             SYMBOLIC COPY_ON_ERROR)
    endforeach()

    install(TARGETS kickmsg_py DESTINATION kickmsg)
    install(
        DIRECTORY ${PY_PACKAGE_SRC}/
        DESTINATION kickmsg
        FILES_MATCHING
            PATTERN "*.py"
            PATTERN "*.pyi"
            PATTERN "py.typed"
    )
endif()
