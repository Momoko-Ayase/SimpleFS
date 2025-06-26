cmake_minimum_required(VERSION 3.10)
project(SimpleFS CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Set include directory
include_directories(include)

# mkfs.simplefs executable
add_executable(mkfs.simplefs
    tools/mkfs.cpp
    src/disk_io.cpp
    src/utils.cpp
)
target_link_libraries(mkfs.simplefs PRIVATE m)

# fsck.simplefs executable
add_executable(fsck.simplefs
    tools/fsck.cpp
    src/disk_io.cpp
    src/utils.cpp
)
target_link_libraries(fsck.simplefs PRIVATE m)


# SimpleFS FUSE daemon
find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE REQUIRED fuse)
include_directories(${FUSE_INCLUDE_DIRS})
link_directories(${FUSE_LIBRARY_DIRS})
add_executable(simplefs
    src/main.cpp
    src/fuse_ops.cpp
    src/disk_io.cpp
    src/metadata.cpp
    src/utils.cpp
)
target_link_libraries(simplefs PRIVATE ${FUSE_LIBRARIES})
# Add required FUSE definitions specifically for simplefs target
target_compile_definitions(simplefs PRIVATE _FILE_OFFSET_BITS=64 FUSE_USE_VERSION=29)


# Enable warnings
if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_CLANG)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -g")
endif()

# Remove global add_definitions for FUSE if they were here, now target-specific

message(STATUS "Project SimpleFS configured (mkfs.simplefs only)")
message(STATUS "CXX Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Source directory: ${CMAKE_SOURCE_DIR}")
message(STATUS "Binary directory: ${CMAKE_BINARY_DIR}")
