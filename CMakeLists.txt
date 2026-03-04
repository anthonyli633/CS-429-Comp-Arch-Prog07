cmake_minimum_required(VERSION 3.16)
project(hw7)

set(CMAKE_C_STANDARD 99)

add_subdirectory(libtdmm)

add_executable(hw7 main.c)
target_link_libraries(hw7 tdmm)
