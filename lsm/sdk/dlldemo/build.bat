@echo off
chcp 65001 >nul
rem Build external demo against lsm_shared.dll: TINYLSM_USE_DLL -> dllimport, link import lib
call "D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo [vcvars] failed & exit /b 1)
cl /nologo /utf-8 /std:c++20 /EHsc /MD /O2 /I D:\tiny-lsm-master\include main.cpp /link /LIBPATH:D:\tiny-lsm-master\build\windows\x64\release lsm_shared.lib /OUT:dlldemo.exe
exit /b %errorlevel%
