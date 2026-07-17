cmake_minimum_required(VERSION 3.25)
project(xex_patch_red_rust LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(xex_patch_red_rust
    mod_main.cpp
)
