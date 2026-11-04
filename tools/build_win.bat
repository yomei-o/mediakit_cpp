@echo off
rem mk_build_dir.bat <builddir> : configure (first time) and build in another directory.
rem cmake options come in %MKOPTS% (cmd splits arguments at '=').
for /f "usebackq delims=" %%i in (`"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -products * -latest -property installationPath`) do set VSDIR=%%i
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d C:\prog\mediakit_cpp
if not exist %1\build.ninja cmake -S . -B %1 -G Ninja -DCMAKE_BUILD_TYPE=Release %MKOPTS% || exit /b 1
start "" /low /b /wait cmake --build %1 -j 10
exit /b %errorlevel%
