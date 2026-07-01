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

# --- Executables / demos ---
add_executable(xrune src/xrune.cpp)
target_link_libraries(xrune PRIVATE xrune_core)

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
