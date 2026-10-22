@echo off
rem Builds RHWind.asi, SAProcObjLimit.asi and RHGrass.asi (x86, static CRT) with Visual Studio 2019/2022 or Build Tools into bin\.
setlocal
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "delims=" %%i in ('vswhere -latest -products * -property installationPath') do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio / Build Tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj
cl /nologo /O2 /MT /EHsc /std:c++17 /DNDEBUG /W4 /LD /Foobj\ src\RHWind.cpp /Fe:bin\RHWind.asi /link /IMPLIB:obj\RHWind.lib user32.lib || exit /b 1
del /q bin\*.exp 2>nul
cl /nologo /O2 /MT /EHsc /std:c++17 /DNDEBUG /W4 /LD /Foobj\ src\SAProcObjLimit.cpp /Fe:bin\SAProcObjLimit.asi /link /IMPLIB:obj\SAProcObjLimit.lib || exit /b 1
del /q bin\*.exp 2>nul
cl /nologo /O2 /MT /EHsc /std:c++17 /DNDEBUG /W4 /LD /Foobj\ src\RHGrass.cpp /Fe:bin\RHGrass.asi /link /IMPLIB:obj\RHGrass.lib user32.lib || exit /b 1
del /q bin\*.exp 2>nul
echo built bin\RHWind.asi, bin\SAProcObjLimit.asi and bin\RHGrass.asi
