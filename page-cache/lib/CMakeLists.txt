cmake_minimum_required(VERSION 3.16)
project(vtpc C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(_GNU_SOURCE)

add_library(vtpc SHARED vtpc.c util.c
        vtpc_locked.c)
target_compile_options(vtpc PRIVATE -Wall -Wextra)
target_link_libraries(vtpc PRIVATE pthread)