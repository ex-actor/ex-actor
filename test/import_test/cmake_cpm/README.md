# Using ex_actor with [CMake Package Manager (CPM)](test/import_test/cmake_cpm)

Run the following commands to get latest CPM.cmake:
```bash
wget -O CPM.cmake https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/get_cpm.cmake
```

Then you can use `CPMAddPackage` to add `ex_actor` as your dependency:

```cmake
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
target_link_libraries(main ex_actor::ex_actor)

```

see [CMakeLists.txt](CMakeLists.txt) and [build.sh](build.sh) in this directory for full example.