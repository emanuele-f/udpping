# CMakeList.txt : CMake project for udpping, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.15)

project("udpping")

set(sources "udpping.c")

IF (WIN32)
  set(sources ${sources} "getopt.c")
ENDIF()

add_executable(udpping ${sources})

IF (WIN32)
  target_compile_options(udpping PRIVATE /W4)
ELSE()
  target_compile_options(udpping PRIVATE -Wall)
  target_link_libraries(udpping pthread)
ENDIF()
