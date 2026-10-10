#!/bin/sh
# Builds and runs the portable tests on Linux/macOS/WSL with g++ (no Windows SDK needed).
# test_core : ini parsing, expressions, command lists, keys, dump formats, hashing
# test_glue : the real Hooks12.cpp / FrameAnalysis12.cpp logic against fake D3D12 objects
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -Wall -Wextra -o test_core test_core.cpp ../Config12.cpp
./test_core
g++ -std=c++17 -g -O0 -Wno-unused-parameter -Ifakewin -I.. -o test_glue test_glue.cpp ../Config12.cpp
rm -rf glue_out && ./test_glue
echo "ALL TESTS PASSED"
