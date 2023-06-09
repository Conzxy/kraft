cmake_minimum_required(VERSION 3.10)
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

project(kraft)

include(cmake/build_type.cmake)
include(cmake/compile_flag.cmake)
include(cmake/proto_util.cmake)
include(cmake/cache_var.cmake)
include(cmake/utils.cmake)

set(CMAKE_CXX_STANDARD 14)

set(3_PARTY_DIR "${PROJECT_SOURCE_DIR}/third-party")

add_subdirectory(kraft)
add_subdirectory(third-party)
  
if (KRAFT_TESTS)
  add_subdirectory(test)
endif ()