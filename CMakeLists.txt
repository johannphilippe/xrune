cmake_minimum_required(VERSION 3.20)

project(xrune LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Fetch the latest RtAudio
FetchContent_Declare(
    rtaudio
    GIT_REPOSITORY https://github.com/thestk/rtaudio.git
    GIT_TAG        master
)

# Fetch Moodycamel ReaderWriterQueue
FetchContent_Declare(
    readerwriterqueue
    GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
    GIT_TAG        master
)

# Avoid building RtAudio tests and unwanted options to keep it lightweight
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(rtaudio readerwriterqueue)

find_package(Threads REQUIRED)

# Optional sanitizer build: -DXRUNE_SANITIZE=thread (or address/undefined).
set(XRUNE_SANITIZE "" CACHE STRING "Build with a sanitizer (thread/address/undefined)")

# Shared configuration for all Xrune targets (header-only library for now).
add_library(xrune_core INTERFACE)
target_include_directories(xrune_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(xrune_core INTERFACE rtaudio readerwriterqueue Threads::Threads)
target_compile_features(xrune_core INTERFACE cxx_std_20)

if(XRUNE_SANITIZE)
    target_compile_options(xrune_core INTERFACE -fsanitize=${XRUNE_SANITIZE} -g -O1)
    target_link_options(xrune_core INTERFACE -fsanitize=${XRUNE_SANITIZE})
endif()

# --- Optional Faust host nodes (dependency policy: core stays dep-free) ---
#   XRUNE_WITH_FAUST       : static node wrapping a Faust-generated dsp class
#                           (needs only Faust's standard architecture headers)
#   XRUNE_WITH_FAUST_LLVM  : JIT node compiling Faust at runtime (needs libfaust)
option(XRUNE_WITH_FAUST "Static Faust node support (Faust standard headers)" OFF)
option(XRUNE_WITH_FAUST_LLVM "JIT Faust node support (libfaust + LLVM backend)" OFF)

if(XRUNE_WITH_FAUST OR XRUNE_WITH_FAUST_LLVM)
    find_path(FAUST_INCLUDE_DIR NAMES faust/dsp/dsp.h
              DOC "Directory containing faust/dsp/dsp.h and faust/gui/APIUI.h")
    if(NOT FAUST_INCLUDE_DIR)
        message(FATAL_ERROR "Faust headers not found (faust/dsp/dsp.h). "
                            "Install Faust or set -DFAUST_INCLUDE_DIR=...")
    endif()
    target_include_directories(xrune_core INTERFACE ${FAUST_INCLUDE_DIR})
endif()

if(XRUNE_WITH_FAUST)
    target_compile_definitions(xrune_core INTERFACE XRUNE_WITH_FAUST)
endif()

if(XRUNE_WITH_FAUST_LLVM)
    find_library(FAUST_LIB NAMES faust libfaust DOC "libfaust (LLVM backend)")
    if(NOT FAUST_LIB)
        message(FATAL_ERROR "libfaust not found. Install Faust or set -DFAUST_LIB=...")
    endif()
    target_link_libraries(xrune_core INTERFACE ${FAUST_LIB})
    target_compile_definitions(xrune_core INTERFACE XRUNE_WITH_FAUST_LLVM)
endif()

# The `faust` compiler, if present, lets the static test generate a dsp header.
find_program(FAUST_COMPILER faust)

# --- Executables / demos ---
add_executable(xrune src/xrune.cpp)
target_link_libraries(xrune PRIVATE xrune_core)

add_executable(galdr_play src/galdr_play.cpp)
target_link_libraries(galdr_play PRIVATE xrune_core)

# --- Tests (CTest, headless via offline backend) ---
enable_testing()

add_executable(test_arena tests/test_arena.cpp)
target_link_libraries(test_arena PRIVATE xrune_core)
add_test(NAME arena COMMAND test_arena)

add_executable(test_offline tests/test_offline.cpp)
target_link_libraries(test_offline PRIVATE xrune_core)
add_test(NAME offline COMMAND test_offline)

add_executable(test_instance tests/test_instance.cpp)
target_link_libraries(test_instance PRIVATE xrune_core)
add_test(NAME instance COMMAND test_instance)

add_executable(test_lifetime tests/test_lifetime.cpp)
target_link_libraries(test_lifetime PRIVATE xrune_core)
add_test(NAME lifetime COMMAND test_lifetime)

add_executable(test_routing tests/test_routing.cpp)
target_link_libraries(test_routing PRIVATE xrune_core)
add_test(NAME routing COMMAND test_routing)

add_executable(test_ports tests/test_ports.cpp)
target_link_libraries(test_ports PRIVATE xrune_core)
add_test(NAME ports COMMAND test_ports)

add_executable(test_multirate tests/test_multirate.cpp)
target_link_libraries(test_multirate PRIVATE xrune_core)
add_test(NAME multirate COMMAND test_multirate)

add_executable(test_parallel tests/test_parallel.cpp)
target_link_libraries(test_parallel PRIVATE xrune_core)
add_test(NAME parallel COMMAND test_parallel)

add_executable(test_rt_safety tests/test_rt_safety.cpp)
target_link_libraries(test_rt_safety PRIVATE xrune_core)
add_test(NAME rt_safety COMMAND test_rt_safety)

add_executable(test_nodes tests/test_nodes.cpp)
target_link_libraries(test_nodes PRIVATE xrune_core)
add_test(NAME nodes COMMAND test_nodes)

add_executable(test_fft tests/test_fft.cpp)
target_link_libraries(test_fft PRIVATE xrune_core)
add_test(NAME fft COMMAND test_fft)

add_executable(test_api tests/test_api.cpp)
target_link_libraries(test_api PRIVATE xrune_core)
add_test(NAME api COMMAND test_api)

add_executable(test_galdr_parse tests/test_galdr_parse.cpp)
target_link_libraries(test_galdr_parse PRIVATE xrune_core)
add_test(NAME galdr_parse COMMAND test_galdr_parse)

add_executable(test_galdr_registry tests/test_galdr_registry.cpp)
target_link_libraries(test_galdr_registry PRIVATE xrune_core)
add_test(NAME galdr_registry COMMAND test_galdr_registry)

add_executable(test_galdr_lower tests/test_galdr_lower.cpp)
target_link_libraries(test_galdr_lower PRIVATE xrune_core)
target_compile_definitions(test_galdr_lower PRIVATE XRUNE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME galdr_lower COMMAND test_galdr_lower)

# --- SIMD gain benchmark (perf comparison + simd_gain==gain correctness) ---
# Built with -O3 -march=native so the intrinsics resolve to this CPU's widest
# ISA (SSE/AVX2/AVX-512) and the standard gain gets auto-vectorised for a fair
# comparison. Registered as a test: it returns non-zero if simd_gain diverges.
add_executable(bench_gain tests/bench_gain.cpp)
target_link_libraries(bench_gain PRIVATE xrune_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bench_gain PRIVATE -O3 -march=native)
endif()
add_test(NAME bench_gain COMMAND bench_gain)

# --- Faust node tests (only when the options are enabled) ---
if(XRUNE_WITH_FAUST_LLVM)
    add_executable(test_faust_jit tests/test_faust_jit.cpp)
    target_link_libraries(test_faust_jit PRIVATE xrune_core)
    add_test(NAME faust_jit COMMAND test_faust_jit)

    add_executable(test_galdr_faust tests/test_galdr_faust.cpp)
    target_link_libraries(test_galdr_faust PRIVATE xrune_core)
    add_test(NAME galdr_faust COMMAND test_galdr_faust)
endif()

if(XRUNE_WITH_FAUST AND FAUST_COMPILER)
    set(_faust_gen ${CMAKE_CURRENT_BINARY_DIR}/generated/faustgain.hpp)
    add_custom_command(
        OUTPUT ${_faust_gen}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
        COMMAND ${FAUST_COMPILER} -lang cpp -double -cn faustgain
                -o ${_faust_gen} ${CMAKE_CURRENT_SOURCE_DIR}/examples/faust/gain.dsp
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/examples/faust/gain.dsp
        COMMENT "faust: generating faustgain.hpp from examples/faust/gain.dsp")
    add_executable(test_faust_static tests/test_faust_static.cpp ${_faust_gen})
    target_include_directories(test_faust_static PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(test_faust_static PRIVATE xrune_core)
    add_test(NAME faust_static COMMAND test_faust_static)
elseif(XRUNE_WITH_FAUST)
    message(WARNING "XRUNE_WITH_FAUST is ON but the 'faust' compiler was not found; "
                    "skipping test_faust_static (the static wrapper header still builds).")
endif()
