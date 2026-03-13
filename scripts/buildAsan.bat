@echo off
if not exist build_asan mkdir build_asan
cd build_asan
cmake -G "MinGW Makefiles" .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build .
cd ..
