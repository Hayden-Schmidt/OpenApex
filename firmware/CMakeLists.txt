cmake_minimum_required(VERSION 3.16)

# firmware/gui is a sibling of main, not nested under it -- register it as a discoverable component.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/gui")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(openapex_terminal)
