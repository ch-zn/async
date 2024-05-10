cmake_minimum_required(VERSION 3.27)
project(async)

set(CMAKE_CXX_STANDARD 20)

add_library(async INTERFACE)
target_sources(async PUBLIC async.hpp)

add_executable(example example.cpp)
target_link_libraries(example async)