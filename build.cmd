@echo off
setlocal
cd /d %~dp0
if not exist build mkdir build
pushd build
cmake -G "Visual Studio 18 2026" -A x64 .. || ( popd & echo [!] CMake configure failed & exit /b 1 )
cmake --build . --config Release -- /m /v:m || ( popd & echo [!] Build failed & exit /b 2 )
popd
echo [+] Built: %~dp0build\Release\YAZDTrainer.exe
endlocal
