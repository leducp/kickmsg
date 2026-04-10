cmake_minimum_required(VERSION 3.15)
project(kickmsg LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_UNIT_TESTS "Build unit tests" OFF)
option(BUILD_EXAMPLES   "Build examples" OFF)
option(BUILD_BENCHMARKS "Build benchmarks (requires Google Benchmark)" OFF)
option(ENABLE_TSAN      "Enable ThreadSanitizer" OFF)

if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

# --- Platform-specific OS sources ---
if(WIN32)
    set(OS_SOURCES
        os/windows/Time.cc
        os/windows/Futex.cc
        os/windows/SharedMemory.cc
    )
    set(OS_LIBRARIES)
else()
    set(OS_SOURCES
        os/linux/Time.cc
        os/linux/Futex.cc
        os/linux/SharedMemory.cc
    )
    set(OS_LIBRARIES rt pthread)
endif()

# --- Library ---
add_library(kickmsg STATIC
    src/types.cc
    src/Publisher.cc
    src/Subscriber.cc
    src/Region.cc
    src/Node.cc
    ${OS_SOURCES}
)

target_include_directories(kickmsg
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/kickmsg
)

target_link_libraries(kickmsg PUBLIC ${OS_LIBRARIES})

# --- Tests ---
if(BUILD_UNIT_TESTS)
    find_package(GTest REQUIRED CONFIG)
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
