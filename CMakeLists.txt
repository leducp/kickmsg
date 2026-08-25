cmake_minimum_required(VERSION 3.15)
project(kickmsg LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find conan-generated CMake package configs in the build directory
list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_BINARY_DIR})

option(BUILD_UNIT_TESTS "Build unit tests" OFF)
option(BUILD_EXAMPLES   "Build examples" OFF)
option(BUILD_BENCHMARKS "Build benchmarks (requires Google Benchmark)" OFF)
option(ENABLE_TSAN      "Enable ThreadSanitizer" OFF)
option(ENABLE_ASAN      "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN     "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_WERROR    "Treat warnings as errors (CI)" OFF)

if(NOT MSVC)
    add_compile_options(-Wall -Wextra)
    if(ENABLE_WERROR)
        add_compile_options(-Werror)
    endif()
endif()

# TSAN cannot share a process with ASAN/UBSAN runtimes; ASAN+UBSAN together is fine.
if(ENABLE_TSAN AND (ENABLE_ASAN OR ENABLE_UBSAN))
    message(FATAL_ERROR "ENABLE_TSAN cannot be combined with ENABLE_ASAN or ENABLE_UBSAN")
endif()

if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -g)
    add_link_options(-fsanitize=address)
endif()

if(ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -g)
    add_link_options(-fsanitize=undefined)
endif()

# --- Platform-specific OS sources ---
if(WIN32)
    set(OS_SOURCES
        src/os/windows/Time.cc
        src/os/windows/Futex.cc
        src/os/windows/SharedMemory.cc
        src/os/windows/Process.cc
    )
    set(OS_LIBRARIES synchronization)
else()
    # Linux and macOS share most of the POSIX OS layer; only sleep(),
    # create(), and the futex backend diverge.
    set(OS_SOURCES
        src/os/posix/Time.cc
        src/os/posix/SharedMemory.cc
        src/os/posix/Process.cc
    )
    if(APPLE)
        list(APPEND OS_SOURCES
            src/os/darwin/Time.cc
            src/os/darwin/SharedMemory.cc
            src/os/darwin/Futex.cc
            src/os/darwin/Process.cc
        )
        set(OS_LIBRARIES pthread)
    else()
        list(APPEND OS_SOURCES
            src/os/linux/Time.cc
            src/os/linux/SharedMemory.cc
            src/os/linux/Futex.cc
            src/os/linux/Process.cc
        )
        set(OS_LIBRARIES rt pthread)
    endif()
endif()

# --- Library ---
add_library(kickmsg STATIC
    src/types.cc
    src/Hash.cc
    src/Naming.cc
    src/Publisher.cc
    src/Subscriber.cc
    src/Region.cc
    src/Registry.cc
    src/Blackboard.cc
    src/Node.cc
    src/os/Process.cc
    ${OS_SOURCES}
)

target_include_directories(kickmsg
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/kickmsg
)

target_link_libraries(kickmsg PUBLIC ${OS_LIBRARIES})

# --- Version header (generated from git: cmake/version.cmake) ---
include(cmake/version.cmake)

# --- Install, pkg-config, find_package ---
include(cmake/install.cmake)

# --- Tests ---
if(BUILD_UNIT_TESTS)
    find_package(GTest REQUIRED CONFIG)
    find_package(argparse REQUIRED CONFIG)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- Examples ---
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# --- Benchmarks ---
if(BUILD_BENCHMARKS)
    find_package(benchmark REQUIRED CONFIG)
    add_subdirectory(benchmarks)
endif()

# --- Python bindings (built only when invoked via scikit-build-core) ---
if(SKBUILD)
    add_subdirectory(py_bindings)
endif()
