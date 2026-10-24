# Building with the compiler bundled in CLion (no downloads, no installs)

1. Find CLion's gcc. In CLion: File > Settings > Build, Execution, Deployment > Toolchains. The default MinGW toolchain shows a "C Compiler" path
   ending in `gcc.exe` (usually `...\CLion ...\bin\mingw\bin\gcc.exe`). Copy that path.
   (Or in File Explorer search your CLion install folder for `gcc.exe`.)
2. Open PowerShell in the project folder (type `powershell` in the folder's address bar) and run:
   `.\build.bat "C:\paste\the\path\to\gcc.exe"`
3. It builds 6 programs into `bin\` and says "Build OK". Then double-click `run_quick.bat`.

If gcc says it cannot find DLLs, run build.bat from CLion's terminal (Terminal tab) instead; that sets up the toolchain PATH.
This build uses -march=native, so the exes only work on the PC that built them: build and run on the same PC.
