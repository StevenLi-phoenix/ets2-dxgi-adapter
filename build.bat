@echo off
rem Builds build\dxgi.dll with the MSVC x64 toolchain (any VS 2019+ with the C++ workload).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found - install Visual Studio with C++ tools & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No Visual Studio with the x64 C++ toolchain found & exit /b 1)
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
ml64 /nologo /c /Fobuild\stubs.obj stubs.asm || exit /b 1
cl /nologo /O2 /EHsc /MT /W3 /c /Fobuild\dxgi_proxy.obj dxgi_proxy.cpp || exit /b 1
link /nologo /DLL /DEF:dxgi.def /OUT:build\dxgi.dll build\dxgi_proxy.obj build\stubs.obj user32.lib || exit /b 1
if "%~1"=="harness" (
  cl /nologo /O2 /EHsc /W3 /Fobuild\harness.obj /Febuild\harness.exe test\harness.cpp || exit /b 1
)
echo built %~dp0build\dxgi.dll
