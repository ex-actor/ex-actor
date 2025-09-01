# Using ex_actor with [CMake Package Manager (CPM)](test/import_test/cmake_cpm)

Run the following commands to get latest CPM.cmake:
```bash
wget -O CPM.cmake https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/get_cpm.cmake
```

Then you can use `CPMAddPackage` to add `ex_actor` as your dependency, see [CMakeLists.txt](CMakeLists.txt) in this directory for example.