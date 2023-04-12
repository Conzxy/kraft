cmake_minimum_required(VERSION 3.10)
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

project(kraft)

include(cmake/build_type.cmake)
include(cmake/compile_flag.cmake)
include(cmake/proto_util.cmake)
include(cmake/cache_var.cmake)
include(cmake/utils.cmake)

set(CMAKE_CXX_STANDARD 14)

include(third-party/protobuf/kraft_protobuf.cmake)
add_subdirectory(kraft)
add_subdirectory(test)
