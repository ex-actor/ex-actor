# Using ex_actor with [CMake Package Manager (CPM)](test/import_test/cmake_cpm)

You may need to add CPM to your project first, see: https://github.com/cpm-cmake/CPM.cmake#adding-cpm

TL;DR; run the following commands to get latest CPM.cmake:
```bash
wget -O CPM.cmake https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/get_cpm.cmake
```

Then you can use `CPMAddPackage` to add ex_actor as your dependencies, see [CMakeLists.txt](CMakeLists.txt).