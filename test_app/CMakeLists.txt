cmake_minimum_required(VERSION 3.16)

# Only build the components we actually need — speeds up the linux target build.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(grocy_unit_tests)
