@echo off

set "build_type=%1"
if NOT DEFINED build_type set "build_type=Debug"

del CMakeCache.txt &^
rd /q /s _deps &^
cmake -A arm64 . -DCMAKE_BUILD_TYPE="%build_type%" &&^
cmake --build . --config "%build_type%" -- -m &&^
del CMakeCache.txt &&^
rd /q /s _deps &&^
cmake . -A x64 -DBUILD_BUNDLE=TRUE -DCMAKE_BUILD_TYPE="%build_type%" &&^
cmake --build . --config "%build_type%" -- -m
