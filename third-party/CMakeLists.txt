# crc32c is vendored as a subproject: keep its tests, benchmarks, glog
# dependency and install rules out of the kraft build.
set(CRC32C_BUILD_TESTS OFF CACHE BOOL "Build crc32c tests" FORCE)
set(CRC32C_BUILD_BENCHMARKS OFF CACHE BOOL "Build crc32c benchmarks" FORCE)
set(CRC32C_USE_GLOG OFF CACHE BOOL "Build crc32c tests with glog" FORCE)
set(CRC32C_INSTALL OFF CACHE BOOL "Install crc32c" FORCE)

add_subdirectory(crc32c)

if (KRAFT_TESTS)
  #add_subdirectory(kanon)
endif ()