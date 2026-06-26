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

add_executable(xrune 
    src/xrune.cpp
    )

add_executable(xrune_parallel_demo 
    src/xrune_parallel_demo.cpp
    )

target_include_directories(xrune 
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

target_include_directories(xrune_parallel_demo 
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

# Link against the fetched libraries
target_link_libraries(xrune PRIVATE rtaudio readerwriterqueue)
target_link_libraries(xrune_parallel_demo PRIVATE rtaudio readerwriterqueue)
