# CMakeList.txt : CMake project for udpping, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)

project ("udpping")

add_executable (udpping "udpping.c" "getopt.c")