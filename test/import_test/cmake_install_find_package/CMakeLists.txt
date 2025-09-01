cmake_minimum_required(VERSION 3.16)
project(my_project LANGUAGES CXX)

# Look for ex_actor (installed with CMake package config files)
find_package(ex_actor REQUIRED)

add_executable(main main.cc)

# Link with ex_actor::ex_actor target provided by the package
target_link_libraries(main PRIVATE ex_actor::ex_actor)