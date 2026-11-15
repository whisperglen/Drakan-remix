@echo off
rem Builds hook\build\ddraw.dll (32-bit) with Visual Studio 2022 Community.
setlocal
call "D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /D_CRT_SECURE_NO_WARNINGS /W3 /O2 /MT /GS- /LD /Fobuild\ /Febuild\ddraw.dll ^
   src\main.c src\proxy.c src\detour.c src\drakan.c src\freecam.c src\texdump.c src\surfspy.c src\comcensus.c src\be_ddraw.c src\be_d9.c ^
   /link /DEF:src\ddraw.def /NOLOGO /SAFESEH:NO user32.lib kernel32.lib dxguid.lib
exit /b %errorlevel%
