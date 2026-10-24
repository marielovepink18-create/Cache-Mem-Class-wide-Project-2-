@echo off
rem Compiles the benchmark tools with a gcc you already have (e.g. the one bundled with CLion).
rem Usage:  build.bat                      (gcc is on PATH)
rem         build.bat "C:\full\path\to\gcc.exe"
cd /d "%~dp0"
set "GCC=gcc"
if not "%~1"=="" set "GCC=%~1"
if not exist bin mkdir bin
"%GCC%" --version >nul 2>&1 || (echo Could not run gcc: "%GCC%" & echo See docs\BUILD_WITH_CLION.md & pause & exit /b 1)
for %%t in (lat bw loaded kernel sysprobe runner) do (
  echo building %%t
  "%GCC%" -O3 -march=native -std=gnu11 -fno-tree-loop-distribute-patterns src\%%t.c -o bin\%%t.exe -ladvapi32 -lm || (echo BUILD FAILED for %%t & pause & exit /b 1)
)
"%GCC%" --version > bin\BUILD_INFO.txt
echo -O3 -march=native -std=gnu11 -fno-tree-loop-distribute-patterns -ladvapi32 -lm >> bin\BUILD_INFO.txt
echo.
echo Build OK. Now double-click run_quick.bat
pause
