@echo off
git submodule update --init External\src\crossguid
mkdir External\build\crossguid
pushd External\build\crossguid
cmake -S ..\..\src\crossguid -B .
cmake --build . --config Release
popd