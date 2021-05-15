#!/bin/bash
cmake -D CMAKE_C_COMPILER=gcc -S. -B./build
ln -sf build/compile_commands.json .
ctags -R --exclude={build,compile_commands.json,makeproject.sh,CMakeLists.txt,doxygen,docs,doxygen.conf} .
#doxygen doxygen.conf
cd build
make
