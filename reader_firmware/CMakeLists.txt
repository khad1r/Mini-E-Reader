cmake_minimum_required(VERSION 3.16)

# Minimal ESP-IDF Project Configuration for Mini E-Reader
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mini_e_reader)
