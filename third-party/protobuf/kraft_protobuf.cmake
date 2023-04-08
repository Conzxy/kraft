function (set_kraft_protobuf_properties target_name lib_path)
  set_target_properties(${target_name} PROPERTIES
    IMPORTED_LOCATION ${lib_path}
    INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}/include
  )
endfunction()

if (UNIX)
  add_library(kraft_protobuf SHARED IMPORTED)
  add_library(kraft_protobuf_lite SHARED IMPORTED)
  add_library(kraft_libprotoc SHARED IMPORTED)
  set(
    kraft-protoc 
    ${CMAKE_CURRENT_LIST_DIR}/bin/linux/protoc
  )

  set_kraft_protobuf_properties(
    kraft_protobuf_lite
    ${CMAKE_CURRENT_LIST_DIR}/lib/linux/libprotobuf-lite.so
  )
  set_kraft_protobuf_properties(
    kraft_protobuf
    ${CMAKE_CURRENT_LIST_DIR}/lib/linux/libprotobuf.so
  )
  set_kraft_protobuf_properties(
    kraft_libprotoc 
    ${CMAKE_CURRENT_LIST_DIR}/lib/linux/libprotoc.so
  )
elseif (WIN32)

endif ()

message(STATUS "kraft protoc = ${kraft-protoc}")
message(STATUS "Current list dir = ${CMAKE_CURRENT_LIST_DIR}")