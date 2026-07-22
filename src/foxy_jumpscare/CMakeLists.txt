cmake_minimum_required(VERSION 3.25)
project(foxy_jumpscare LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(foxy_jumpscare
    mod_main.cpp
)
