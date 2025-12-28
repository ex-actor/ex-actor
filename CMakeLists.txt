cmake_minimum_required(VERSION 3.25.0 FATAL_ERROR)
project(ex_actor VERSION 0.1)
include(cmake/CPM.cmake)
include(CMakePackageConfigHelpers)

# ------------------- global setups -------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-fdiagnostics-color=always)
endif()
set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS 1 CACHE INTERNAL "")
if(MSVC)
  add_compile_options(/EHsc)
  add_compile_options(/bigobj)
endif()
# ------------------- dependencies -------------------

# When C++26 is ready, I'll add a build option to switch to the real std::execution in C++26,
# allow you to remove the dependency on stdexec. Stay tuned!
CPMAddPackage(
  NAME stdexec
  GITHUB_REPOSITORY NVIDIA/stdexec
  GIT_TAG 770f7959142c2446319f642c82aed8b07aea3f86
  OPTIONS "BUILD_TESTING OFF"
  OPTIONS "STDEXEC_BUILD_EXAMPLES OFF"
)

# reflect-cpp with Cap'n Proto for actor message serialization
CPMAddPackage(
  NAME zlib-cmake
  GITHUB_REPOSITORY jimmy-park/zlib-cmake
  GIT_TAG edc9d59d34ea33b1fae4f40268bc69a6d5dd70c0
  OPTIONS "ZLIB_TEST OFF"
  OPTIONS "ZLIB_INSTALL ON"
)
CPMAddPackage(
  NAME CapnProto
  GITHUB_REPOSITORY capnproto/capnproto
  GIT_TAG v1.2.0
  OPTIONS "BUILD_TESTING OFF"
  OPTIONS "CMAKE_CXX_FLAGS -w"
)
CPMAddPackage(
  NAME reflectcpp
  GITHUB_REPOSITORY getml/reflect-cpp
  GIT_TAG 2b1cd836776c55646d1bc92204da1362ed652bda
  OPTIONS "REFLECTCPP_BUILD_BENCHMARKS OFF"
  OPTIONS "REFLECTCPP_JSON OFF"
  OPTIONS "REFLECTCPP_CAPNPROTO ON"
  OPTIONS "REFLECTCPP_INSTALL ON"
)

# libzmq + cppzmq for message passing in distributed mode
CPMAddPackage(
  NAME libzmq
  GITHUB_REPOSITORY zeromq/libzmq
  GIT_TAG v4.3.5
  OPTIONS "CMAKE_CXX_FLAGS -w"
  OPTIONS "BUILD_TESTS OFF"
  OPTIONS "BUILD_SHARED OFF"
  OPTIONS "WITH_TLS OFF"
  OPTIONS "WITH_LIBSODIUM OFF"
  OPTIONS "WITH_GSSAPI OFF"
)
CPMAddPackage(
  NAME cppzmq
  GITHUB_REPOSITORY zeromq/cppzmq
  GIT_TAG v4.11.0
  OPTIONS "CPPZMQ_BUILD_TESTS OFF"
)

CPMAddPackage(
  NAME spdlog
  GITHUB_REPOSITORY gabime/spdlog
  GIT_TAG v1.16.0
  OPTIONS "SPDLOG_INSTALL ON"
)

# ------------------- targets -------------------
add_library(
  ex_actor
  STATIC
  src/ex_actor/internal/network.cc
  src/ex_actor/internal/actor_registry.cc
  src/ex_actor/internal/logging.cc
)
target_include_directories(ex_actor PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(
  ex_actor
  PUBLIC
  STDEXEC::stdexec
  reflectcpp::reflectcpp
  cppzmq-static
  spdlog::spdlog
)
# to suppress subobject linkage warnings in stdexec
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  target_compile_options(ex_actor INTERFACE -Wno-subobject-linkage)
endif()
add_library(ex_actor::ex_actor ALIAS ex_actor)

# ------------------- tests -------------------
option(EX_ACTOR_BUILD_TESTS "Build tests" ON)
if(EX_ACTOR_BUILD_TESTS)
  CPMAddPackage(
    NAME googletest
    GITHUB_REPOSITORY google/googletest
    GIT_TAG b2b9072ecbe874f5937054653ef8f2731eb0f010
    OPTIONS
    "INSTALL_GTEST OFF"
    "gtest_force_shared_crt ON"
  )
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
