cmake_minimum_required(VERSION 3.25.0 FATAL_ERROR)
project(ex_actor VERSION 0.1)
include(cmake/CPM.cmake)
include(CMakePackageConfigHelpers)

# ------------------- global setups -------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  add_compile_options(-Wno-subobject-linkage)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-fdiagnostics-color=always)
endif()

# ------------------- dependencies -------------------
CPMAddPackage(
  NAME stdexec
  GITHUB_REPOSITORY NVIDIA/stdexec
  GIT_TAG 770f7959142c2446319f642c82aed8b07aea3f86
  OPTIONS "BUILD_TESTING OFF"
  OPTIONS "STDEXEC_BUILD_EXAMPLES OFF"
)
# ------------------- targets -------------------
add_library(ex_actor INTERFACE)
target_include_directories(ex_actor INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(
  ex_actor
  INTERFACE STDEXEC::stdexec
)
# to suppress subobject linkage warnings in stdexec
target_compile_options(ex_actor INTERFACE -Wno-subobject-linkage)
add_library(ex_actor::ex_actor ALIAS ex_actor)

# ------------------- tests -------------------
option(EX_ACTOR_BUILD_TESTS "Build tests" ON)
if(EX_ACTOR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

# ------------------- install -------------------
install(TARGETS ex_actor EXPORT ex_actor-targets)
install(DIRECTORY include/ DESTINATION include)
install(
  EXPORT ex_actor-targets
  NAMESPACE ex_actor::
  DESTINATION lib/cmake/ex_actor
)

configure_package_config_file(
  cmake/ex_actor-config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ex_actor-config.cmake
  INSTALL_DESTINATION lib/cmake/ex_actor
)

install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/ex_actor-config.cmake
  DESTINATION lib/cmake/ex_actor
)
