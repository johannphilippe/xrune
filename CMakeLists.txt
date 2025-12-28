cmake_minimum_required(VERSION 3.20)

include(FetchContent)

project(xrune LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/cmake/Modules/)

option(TESTS "Build test executable" ON)
option(BUILD_FAUST_JIT_NODE "Build Faust JIT (require LLVM) node" ON)
option(BUILD_CSOUND_NODE "Build Csound node (also enables Csound graph)" ON)
option(BUILD_SNDFILE_NODE "Build Sndfile nodes" ON)

add_executable(xrune 
    src/xrune.cpp
    src/player.hpp
    src/tui.hpp
    )


add_subdirectory(src/combinator3000)

target_include_directories(xrune 
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/combinator3000/src
    )

set(XRUNE_LIBRARIES combinator3000)
if(BUILD_CSOUND_NODE)
    find_package(Csound REQUIRED)
    target_compile_definitions(xrune PUBLIC CSOUND_GRAPH)
    set(XRUNE_LIBRARIES ${XRUNE_LIBRARIES} ${CSOUND_LIBRARIES})
endif()

target_link_libraries(
    xrune PUBLIC ${XRUNE_LIBRARIES}
)
