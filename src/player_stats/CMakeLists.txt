cmake_minimum_required(VERSION 3.25)
project(player_stats LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(player_stats
    mod_main.cpp
)
