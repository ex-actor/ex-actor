cmake_minimum_required(VERSION 3.25.0 FATAL_ERROR)
project(my_project)

# For more information on how to add CPM to your project, see: https://github.com/cpm-cmake/CPM.cmake#adding-cpm
include(CPM.cmake)

CPMAddPackage(
  NAME ex_actor
  GITHUB_REPOSITORY ex-actor/ex-actor
  GIT_TAG main # This will always pull the latest code from the `main` branch. You may also use a specific commit ID
  OPTIONS "EX_ACTOR_BUILD_TESTS OFF"
)

add_executable(main main.cc)
target_link_libraries(main ex_actor)