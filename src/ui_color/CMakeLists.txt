cmake_minimum_required(VERSION 3.25)
project(ui_color LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(ui_color
    mod_main.cpp
)
