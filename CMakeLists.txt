cmake_minimum_required(VERSION 3.20)

project(xrune VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
FetchContent_Declare(rtaudio
    GIT_REPOSITORY https://github.com/thestk/rtaudio.git
    GIT_TAG        master)
FetchContent_Declare(readerwriterqueue
    GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
    GIT_TAG        master)

set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rtaudio readerwriterqueue)

find_package(Threads REQUIRED)

# Optional sanitizer build: -DXRUNE_SANITIZE=thread (or address/undefined).
set(XRUNE_SANITIZE "" CACHE STRING "Build with a sanitizer (thread/address/undefined)")

option(XRUNE_BUILD_TESTS "Build the test suite" ON)
option(XRUNE_BUILD_APPS  "Build the example applications" ON)

# ---------------------------------------------------------------------------
# Optional Faust host nodes (dependency policy: the core stays dependency-free)
#   XRUNE_WITH_FAUST      : static node wrapping a Faust-generated dsp class
#                           (needs only Faust's standard architecture headers)
#   XRUNE_WITH_FAUST_LLVM : JIT node compiling Faust at runtime (needs libfaust)
# ---------------------------------------------------------------------------
option(XRUNE_WITH_FAUST "Static Faust node support (Faust standard headers)" OFF)
option(XRUNE_WITH_FAUST_LLVM "JIT Faust node support (libfaust + LLVM backend)" OFF)

if(XRUNE_WITH_FAUST OR XRUNE_WITH_FAUST_LLVM)
    find_path(FAUST_INCLUDE_DIR NAMES faust/dsp/dsp.h
              DOC "Directory containing faust/dsp/dsp.h and faust/gui/APIUI.h")
    if(NOT FAUST_INCLUDE_DIR)
        message(FATAL_ERROR "Faust headers not found (faust/dsp/dsp.h). "
                            "Install Faust or set -DFAUST_INCLUDE_DIR=...")
    endif()
endif()

if(XRUNE_WITH_FAUST_LLVM)
    find_library(FAUST_LIB NAMES faust libfaust DOC "libfaust (LLVM backend)")
    if(NOT FAUST_LIB)
        message(FATAL_ERROR "libfaust not found. Install Faust or set -DFAUST_LIB=...")
    endif()
endif()

# The `faust` compiler, if present, lets the static test generate a dsp header.
find_program(FAUST_COMPILER faust)

# ---------------------------------------------------------------------------
# The library
#
# Headers live in include/xrune/, so users write  #include <xrune/api.hpp>.
# Implementation lives in src/. Files with substantial non-template code are
# compiled here; the rest stay header-only *on purpose*:
#   core.hpp      the node interface third parties derive from
#   blueprint.hpp templates (add<T>)
#   util/arena, util/mpmc_queue, util/rt_check   RT-hot, must inline
#   node/faust/*  templates
# ---------------------------------------------------------------------------
add_library(xrune
    src/api.cpp
    src/engine.cpp
    src/schedule.cpp
    src/serialize.cpp
    src/util/json.cpp
)
add_library(xrune::xrune ALIAS xrune)

target_include_directories(xrune PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(xrune PUBLIC rtaudio readerwriterqueue Threads::Threads)
target_compile_features(xrune PUBLIC cxx_std_20)

if(XRUNE_SANITIZE)
    target_compile_options(xrune PUBLIC -fsanitize=${XRUNE_SANITIZE} -g -O1)
    target_link_options(xrune PUBLIC -fsanitize=${XRUNE_SANITIZE})
endif()

if(XRUNE_WITH_FAUST OR XRUNE_WITH_FAUST_LLVM)
    target_include_directories(xrune PUBLIC ${FAUST_INCLUDE_DIR})
endif()
if(XRUNE_WITH_FAUST)
    target_compile_definitions(xrune PUBLIC XRUNE_WITH_FAUST)
endif()
if(XRUNE_WITH_FAUST_LLVM)
    target_link_libraries(xrune PUBLIC ${FAUST_LIB})
    target_compile_definitions(xrune PUBLIC XRUNE_WITH_FAUST_LLVM)
endif()

# Kept so existing targets keep working; xrune::xrune is the name to use.
add_library(xrune_core INTERFACE)
target_link_libraries(xrune_core INTERFACE xrune)

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
if(XRUNE_BUILD_APPS)
    add_executable(xrune_app apps/xrune.cpp)
    set_target_properties(xrune_app PROPERTIES OUTPUT_NAME xrune_demo)
    target_link_libraries(xrune_app PRIVATE xrune::xrune)

    add_executable(xrune_play apps/xrune_play.cpp)
    target_link_libraries(xrune_play PRIVATE xrune::xrune)
endif()

# ---------------------------------------------------------------------------
# Install / export  ->  find_package(xrune) + #include <xrune/api.hpp>
# ---------------------------------------------------------------------------
install(TARGETS xrune xrune_core
        EXPORT xruneTargets
        LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/xrune
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")

install(EXPORT xruneTargets
        FILE xruneTargets.cmake
        NAMESPACE xrune::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xrune)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/xruneConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/xruneConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/xruneConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xrune)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/xruneConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/xruneConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xrune)

# ---------------------------------------------------------------------------
# Tests (CTest, headless via the offline backend)
# ---------------------------------------------------------------------------
if(XRUNE_BUILD_TESTS)
    enable_testing()

    set(XRUNE_TESTS
        arena offline instance nodes ports multirate routing lifetime
        parallel rt_safety fft api serialize
        lang_parse lang_registry lang_lower)

    foreach(t IN LISTS XRUNE_TESTS)
        add_executable(test_${t} tests/test_${t}.cpp)
        target_link_libraries(test_${t} PRIVATE xrune::xrune)
        target_include_directories(test_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()

    target_compile_definitions(test_lang_lower PRIVATE
        XRUNE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

    # --- Faust node tests (only when the options are enabled) ---
    if(XRUNE_WITH_FAUST_LLVM)
        foreach(t IN ITEMS faust_jit lang_faust)
            add_executable(test_${t} tests/test_${t}.cpp)
            target_link_libraries(test_${t} PRIVATE xrune::xrune)
            target_include_directories(test_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
            add_test(NAME ${t} COMMAND test_${t})
        endforeach()
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
        target_include_directories(test_faust_static PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/generated ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_link_libraries(test_faust_static PRIVATE xrune::xrune)
        add_test(NAME faust_static COMMAND test_faust_static)
    elseif(XRUNE_WITH_FAUST)
        message(WARNING "XRUNE_WITH_FAUST is ON but the 'faust' compiler was not found; "
                        "skipping test_faust_static (the static wrapper header still builds).")
    endif()
endif()

# ---------------------------------------------------------------------------
# SIMD exploration area (src/node/simd_nodes/) — a personal sandbox, NOT tracked
# in git. Entirely optional: the project must configure, build and test the same
# whether or not it is present, and no tracked source may include it.
# ---------------------------------------------------------------------------
set(XRUNE_SIMD_BENCH ${CMAKE_CURRENT_SOURCE_DIR}/src/node/simd_nodes/bench_gain.cpp)
if(EXISTS ${XRUNE_SIMD_BENCH})
    message(STATUS "simd_nodes/ present -- building bench_gain")
    add_executable(bench_gain ${XRUNE_SIMD_BENCH})
    target_link_libraries(bench_gain PRIVATE xrune::xrune)
    target_include_directories(bench_gain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(bench_gain PRIVATE -O3 -march=native)
    endif()
    if(XRUNE_BUILD_TESTS)
        add_test(NAME bench_gain COMMAND bench_gain)
    endif()
else()
    message(STATUS "simd_nodes/ absent -- skipping bench_gain (optional sandbox)")
endif()
