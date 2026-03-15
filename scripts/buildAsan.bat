@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "ROOT_DIR=%%~fI"
set "BUILD_DIR=%ROOT_DIR%\build_asan"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if exist "%BUILD_DIR%\CMakeCache.txt" del /Q "%BUILD_DIR%\CMakeCache.txt"
if exist "%BUILD_DIR%\CMakeFiles" rmdir /S /Q "%BUILD_DIR%\CMakeFiles"
for /D %%D in ("%BUILD_DIR%\_deps\*-subbuild") do (
    if exist "%%D\CMakeCache.txt" del /Q "%%D\CMakeCache.txt"
)
cmake -S "%ROOT_DIR%" -B "%BUILD_DIR%" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build "%BUILD_DIR%"
