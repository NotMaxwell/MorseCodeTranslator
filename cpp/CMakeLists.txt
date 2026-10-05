cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Only build the components main actually depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(morse_code_translator)
