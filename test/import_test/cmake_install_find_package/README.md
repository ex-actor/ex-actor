# Using ex_actor with CMake's install and find_package

first, install ex_actor to your system:

```bash
git clone https://github.com/ex-actor/ex-actor.git --depth 1 && cd ex-actor
cmake -S . -B build -G "Ninja Multi-Config"
cmake --build build --config Release
cmake --install build --prefix <your_install_prefix>
```

then, you can use `find_package` to add ex_actor as your dependencies, don't forget to set `CMAKE_PREFIX_PATH` to your install prefix, see [CMakeLists.txt](CMakeLists.txt).