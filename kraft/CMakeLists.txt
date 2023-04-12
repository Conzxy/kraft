set(PB_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
message(STATUS "kraft pb code files output dir = ${PB_OUTPUT_DIR}")

gen_proto_code(PROTOC_PATH ${kraft-protoc} FILES "${CMAKE_CURRENT_SOURCE_DIR}/kraft.proto")

file(GLOB_RECURSE
  KRAFT_SOURCES
  *.cc
)

kraft_gen_lib(kraft
  ${KRAFT_SOURCES}
  ${PB_OUTPUT_DIR}/kraft.pb.cc
)

target_include_directories(
  kraft
  PRIVATE ${PB_OUTPUT_DIR}
  PUBLIC 
  "$<BUILD_INTERFACE:${CMAKE_PROJECT_SOURCE_DIR}>"
  "$<INSTALL_INTERFACE: ${CMAKE_INSTALL_INCLUDEDIR}>"
)

# Install files...
