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

    # Source layout:
    #   py_bindings/src/kickmsg_py.cc    C++ binding (compiled to _native.so)
    #   python/kickmsg/                  pure-Python package
    #
    # At install / editable-build time both land in the same package dir.
    set_target_properties(kickmsg_py PROPERTIES
        OUTPUT_NAME "_native"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/kickmsg")

    # Stage pure-Python sources alongside the native module for
    # PYTHONPATH-based dev iteration.  Symlinks so .py edits don't
    # require a rebuild.
    set(PY_PACKAGE_SRC ${CMAKE_SOURCE_DIR}/python/kickmsg)
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
