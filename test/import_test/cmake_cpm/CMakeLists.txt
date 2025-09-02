cmake_minimum_required(VERSION 3.25.0 FATAL_ERROR)
project(my_project)

# For more information on how to add CPM to your project, see: https://github.com/cpm-cmake/CPM.cmake#adding-cpm
include(CPM.cmake)

CPMAddPackage(
  NAME ex_actor
  GITHUB_REPOSITORY ex-actor/ex-actor
  # For CI test, we use the current commit ID. You should use a fixed commit ID.
  GIT_TAG $ENV{GITHUB_REF}
  OPTIONS "EX_ACTOR_BUILD_TESTS OFF"
)

add_executable(main main.cc)
target_link_libraries(main ex_actor::ex_actor)
