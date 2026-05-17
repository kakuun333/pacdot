cmake_minimum_required(VERSION 3.20)

project(pacdot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(pacdot
    Source/main.cpp
)

target_include_directories(pacdot PRIVATE
    Dependencies/glob-cpp/include
    Dependencies/tomlplusplus/include
)
