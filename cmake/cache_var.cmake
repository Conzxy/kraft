# Check if kraft is being used directly or via add_subdirectory, but allow overriding
if(NOT DEFINED KRAFT_MAIN_PROJECT)
    if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
        set(KRAFT_MAIN_PROJECT ON)
    else()
        set(KRAFT_MAIN_PROJECT OFF)
    endif()
endif()

option(KRAFT_BUILD_STATIC_LIBS "Build static library of kraft" ON)
# to install files to proper destination actually.
option(KRAFT_INSTALL "Generate the install target" ${KRAFT_MAIN_PROJECT})

option(KRAFT_TESTS "Generate kraft test targets" OFF)

# User can determine whether to build all tests when build target all
# e.g. cmake --build */KRAFT/build [--target all -j 2]
# If this option is OFF, user should specify target manually.
option(KRAFT_BUILD_ALL_TESTS "Build tests when --target all(default) is specified" OFF)
