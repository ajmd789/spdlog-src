#!/bin/bash
mkdir -p build_asan
cd build_asan
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build . --config Debug
