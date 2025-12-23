cmake_minimum_required(VERSION 3.20)

include(FetchContent)

project(xrune LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/cmake/Modules/)

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

target_link_libraries(
    xrune PUBLIC combinator3000
)