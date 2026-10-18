cmake_minimum_required(VERSION 3.20)
project(MyGame LANGUAGES CXX)
find_package(SaidaEngine CONFIG REQUIRED)
saida_add_game(MyGame WITH_EDITOR PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
