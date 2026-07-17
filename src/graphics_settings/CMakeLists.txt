cmake_minimum_required(VERSION 3.25)
project(graphics_settings LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(graphics_settings
    mod_main.cpp
)
