# Development

## How to Build from Source

```bash
cd ex_actor
./scripts/regen_build_dir.sh

cd build
#build
cmake --build . --config Release
# install
cmake --install . --prefix <your_install_prefix>
# test
ctest -C Release --output-on-failure
```