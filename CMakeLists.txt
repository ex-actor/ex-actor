cmake_minimum_required(VERSION 3.25.0 FATAL_ERROR)
project(ex_actor VERSION 0.1)
include(cmake/CPM.cmake)

# ------------------- global setups -------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wno-subobject-linkage) # to suppress subobject linkage warnings in stdexec

# ------------------- dependencies -------------------
CPMAddPackage(
  NAME stdexec
  GITHUB_REPOSITORY NVIDIA/stdexec
  GIT_TAG ac2d378944b68021a3c51ed98987b6d4cf05b989
)

# ------------------- targets -------------------
add_library(ex_actor INTERFACE)
target_include_directories(ex_actor INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(
  ex_actor
  INTERFACE STDEXEC::stdexec
)

# ------------------- tests -------------------
enable_testing()
add_executable(basic_api_test test/basic_api_test.cc)
target_link_libraries(basic_api_test ex_actor)
add_test(NAME basic_api_test COMMAND basic_api_test)

# ------------------- install -------------------
install(TARGETS ex_actor EXPORT ex_actor_targets)
install(DIRECTORY include/ DESTINATION include)
install(
  EXPORT ex_actor_targets
  NAMESPACE ex_actor::
  DESTINATION lib/cmake/ex_actor
)
