function (gen_proto_code)
  cmake_parse_arguments(PARSE_ARGV 0 CONZXY "" "PROTOC_PATH" "FILES")
  message(STATUS "protoc path = ${CONZXY_PROTOC_PATH}")
  foreach (proto_file_fullpath ${CONZXY_FILES})
    get_filename_component(proto_filename ${proto_file_fullpath} NAME_WE)
    get_filename_component(proto_dir ${proto_file_fullpath} DIRECTORY)
    add_custom_command(
      OUTPUT ${proto_filename}.pb.cc ${proto_filename}.pb.h
      COMMAND ${CONZXY_PROTOC_PATH}
      ARGS --cpp_out . ${proto_file_fullpath} -I${proto_dir}
      VERBATIM
    )
  endforeach ()
endfunction ()