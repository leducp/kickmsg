# Generate <build>/generated/kickmsg/version.h from cmake/version.h.in, with the
# version + git metadata derived from `git describe`. The header is regenerated
# on every build so the stamp tracks the working tree (commits, dirty state).
#
# Two modes:
#   - included from CMakeLists (configure time): generates the header now and
#     wires a build-time regeneration target onto the kickmsg library.
#   - cmake -D KICKMSG_VERSION_SRC=<src> -D KICKMSG_VERSION_OUT=<file> -P version.cmake
#     (build time): re-runs git and rewrites the header.
#
# With no git available (source tarball / conan-from-recipe) it falls back to
# 0.0.0 and "unknown" git fields, unless -D KICKMSG_VERSION_OVERRIDE=X.Y.Z is
# passed (e.g. by a package recipe that knows the release version).

function(_kickmsg_compute_version src_dir)
    set(_describe "unknown")
    set(_branch "unknown")
    set(_tag "")
    set(_hash "unknown")
    set(_dirty 0)
    set(_ver "0.0.0")

    if(EXISTS "${src_dir}/.git")
        execute_process(COMMAND git describe --always --tags --dirty
            WORKING_DIRECTORY "${src_dir}" OUTPUT_VARIABLE _describe
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND git rev-parse --abbrev-ref HEAD
            WORKING_DIRECTORY "${src_dir}" OUTPUT_VARIABLE _branch
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND git describe --tags --exact-match
            WORKING_DIRECTORY "${src_dir}" OUTPUT_VARIABLE _tag
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND git rev-parse --short HEAD
            WORKING_DIRECTORY "${src_dir}" OUTPUT_VARIABLE _hash
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND git status --porcelain
            WORKING_DIRECTORY "${src_dir}" OUTPUT_VARIABLE _status
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_status)
            set(_dirty 1)
        endif()
        # Nearest tag -> clean MAJOR.MINOR.PATCH for packaging/version string.
        execute_process(COMMAND git describe --tags --abbrev=0
            WORKING_DIRECTORY "${src_dir}" OUTPUT_VARIABLE _nearest
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        string(REGEX REPLACE "^v" "" _nearest "${_nearest}")
        if(_nearest MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
            set(_ver "${_nearest}")
        endif()
    endif()

    # Explicit override (release tarball / packaging, where no .git exists).
    if(KICKMSG_VERSION_OVERRIDE)
        string(REGEX REPLACE "^v" "" _override "${KICKMSG_VERSION_OVERRIDE}")
        if(_override MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
            set(_ver "${_override}")
            if(_describe STREQUAL "unknown")
                set(_describe "v${_ver}")
            endif()
            if(_tag STREQUAL "")
                set(_tag "v${_ver}")
            endif()
        endif()
    endif()

    if(_ver MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        set(_maj "${CMAKE_MATCH_1}")
        set(_min "${CMAKE_MATCH_2}")
        set(_pat "${CMAKE_MATCH_3}")
    else()
        set(_maj 0)
        set(_min 0)
        set(_pat 0)
    endif()

    set(KICKMSG_VERSION_STRING "${_ver}"      PARENT_SCOPE)
    set(KICKMSG_VERSION_MAJOR  "${_maj}"      PARENT_SCOPE)
    set(KICKMSG_VERSION_MINOR  "${_min}"      PARENT_SCOPE)
    set(KICKMSG_VERSION_PATCH  "${_pat}"      PARENT_SCOPE)
    set(KICKMSG_VERSION        "${_maj}.${_min}.${_pat}" PARENT_SCOPE)
    set(KICKMSG_GIT_DESCRIBE   "${_describe}" PARENT_SCOPE)
    set(KICKMSG_GIT_BRANCH     "${_branch}"   PARENT_SCOPE)
    set(KICKMSG_GIT_TAG        "${_tag}"      PARENT_SCOPE)
    set(KICKMSG_GIT_COMMIT_HASH "${_hash}"    PARENT_SCOPE)
    set(KICKMSG_GIT_DIRTY      "${_dirty}"    PARENT_SCOPE)
endfunction()

# --- Build-time mode (cmake -P) ---
if(KICKMSG_VERSION_SRC)
    _kickmsg_compute_version("${KICKMSG_VERSION_SRC}")
    configure_file("${KICKMSG_VERSION_SRC}/cmake/version.h.in"
                   "${KICKMSG_VERSION_OUT}" @ONLY)
    return()
endif()

# --- Configure-time mode (include() from CMakeLists, after project()) ---
_kickmsg_compute_version("${CMAKE_CURRENT_SOURCE_DIR}")

set(KICKMSG_VERSION_H "${CMAKE_BINARY_DIR}/generated/kickmsg/version.h")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in"
               "${KICKMSG_VERSION_H}" @ONLY)

set(_version_deps "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
    list(APPEND _version_deps "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/index")
    list(APPEND _version_deps "${CMAKE_CURRENT_SOURCE_DIR}/.git/index")
endif()

add_custom_command(
    OUTPUT "${KICKMSG_VERSION_H}"
    COMMAND ${CMAKE_COMMAND}
        -D KICKMSG_VERSION_SRC=${CMAKE_CURRENT_SOURCE_DIR}
        -D KICKMSG_VERSION_OUT=${KICKMSG_VERSION_H}
        -D KICKMSG_VERSION_OVERRIDE=${KICKMSG_VERSION_OVERRIDE}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake"
    DEPENDS ${_version_deps}
    COMMENT "Regenerating kickmsg/version.h from git"
    VERBATIM
)
add_custom_target(kickmsg_version DEPENDS "${KICKMSG_VERSION_H}")

# Expose the generated header on the library's public include path and make
# sure it is generated before anything that includes it compiles.
target_include_directories(kickmsg PUBLIC
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>)
add_dependencies(kickmsg kickmsg_version)
