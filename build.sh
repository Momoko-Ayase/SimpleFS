#!/bin/bash
cmake --preset default
cd build
ninja
if [ $? -eq 0 ]; then
    echo "Build successful!"
else
    echo "Build failed!"
    exit 1
fi