set(PB_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
message(STATUS "kraft pb code files output dir = ${PB_OUTPUT_DIR}")

find_package(Protobuf REQUIRED)
# Requires this protoc to generate kraft.pb.*
# User must link higher version protobuf lib to against this if 
# the `KRAFT_LINK_PROTOBUF` is specified as OFF
gen_proto_code(PROTOC_PATH ${PROTOBUF_PROTOC_EXECUTABLE} FILES "${CMAKE_CURRENT_SOURCE_DIR}/kraft.proto")

file(GLOB_RECURSE
  KRAFT_SOURCES
  error_code.cc
  util/*.cc
  storage/raft_log_buffer.cc
  storage/file.cc
  storage/dir.cc
  storage/raft_log_buffer_pool.cc
  storage/buffer_pool_log.cc
  kraft.cc
  ${3_PARTY_DIR}/kerror/kerror.cc 
)

kraft_gen_lib(kraft
  ${KRAFT_SOURCES}
  ${PB_OUTPUT_DIR}/kraft.pb.cc
)

if (KRAFT_LINK_PROTOBUF)
  target_link_libraries(kraft PUBLIC protobuf::libprotobuf)
else ()
  if ("${kraft_protobuf_to_link}" STREQUAL "")
    message(FATAL_ERROR "User must specify the kraft_protobuf_to_link variable for link")
  endif ()
  target_link_libraries(kraft PUBLIC kraft_protobuf_to_link)
endif ()

target_include_directories(
  kraft
  PUBLIC
  ${PB_OUTPUT_DIR}
  PUBLIC 
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>"
  "$<INSTALL_INTERFACE: ${CMAKE_INSTALL_INCLUDEDIR}>" # FIXME project-specific install dir
  PUBLIC
  "$<BUILD_INTERFACE:${3_PARTY_DIR}>"
  "$<INSTALL_INTERFACE: ${CMAKE_INSTALL_INCLUDEDIR}/third-party>"
)

# Install files...
