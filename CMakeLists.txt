cmake_minimum_required(VERSION 3.24)
project(inputdelay CXX)

set(CMAKE_CXX_STANDARD 17)

find_package(SDL3 REQUIRED)

add_executable(inputdelay main.cpp PointerPredictor.cpp)
target_link_libraries(inputdelay PRIVATE SDL3::SDL3)
