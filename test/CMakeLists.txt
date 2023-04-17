#enable_testing()
file(GLOB KRAFT_TEST_SOURCES "${PROJECT_SOURCE_DIR}/test/*/*test*.cc")
file(GLOB KRAFT_BENCH_SOURCES "${PROJECT_SOURCE_DIR}/test/*/*bench*.cc")

message(STATUS "kraft test source files: ${KRAFT_TEST_SOURCES}")
message(STATUS "kraft bench source files: ${KRAFT_BENCH_SOURCES}")

if (NOT ${BUILD_ALL_TESTS})
  message(STATUS "[The kraft test and bench files are excluded from the build of target all]")
endif (NOT ${BUILD_ALL_TESTS})

message(STATUS "kraft protoc = ${kraft-protoc}")
############################################
# generate test and bench program
############################################
macro (GenTest SRC)
  foreach (kraft_test_source ${${SRC}})
    get_filename_component(kraft_test_filename ${kraft_test_source} NAME_WE)
    get_filename_component(kraft_test_file_dir ${kraft_test_source} DIRECTORY)
    
    set(test_sources "")
    list(APPEND test_sources ${kraft_test_source})
    set(proto_file "${kraft_test_file_dir}/${kraft_test_filename}.proto")
    if (EXISTS ${proto_file})
      gen_proto_code(PROTOC_PATH ${kraft-protoc} FILES ${proto_file})
      list(APPEND test_sources ${kraft_test_filename}.pb.cc)
    endif ()

    if (${BUILD_ALL_TESTS})
      add_executable(${kraft_test_filename} ${test_sources})
    else ()
      message(STATUS "${kraft_test_filename} is excluded from target all")
      add_executable(${kraft_test_filename} EXCLUDE_FROM_ALL ${test_sources})
    endif (${BUILD_ALL_TESTS})
  
    target_link_libraries(${kraft_test_filename} PRIVATE ${ARGN})
    target_include_directories(
      ${kraft_test_filename}
      PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
      PRIVATE ${PROJECT_SOURCE_DIR}  
    )
    set_target_properties(${kraft_test_filename}
      PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test"
      COMMAND ${kraft_test_filename}
    )

    add_test(NAME ${kraft_test_filename} COMMAND ${kraft_test_filename})
  endforeach (kraft_test_source ${KRAFT_TEST_SOURCES})
endmacro()

find_package(GTest CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)

set(TEST_LIBS kraft kraft_protobuf)
set(BENCH_LIBS kraft kraft_protobuf)

GenTest(KRAFT_TEST_SOURCES GTest::gtest GTest::gtest_main ${TEST_LIBS})
GenTest(KRAFT_BENCH_SOURCES benchmark benchmark_main ${BENCH_LIBS})
