@echo off
setlocal
REM Windows 上用 MSVC 编译。先开 "x64 Native Tools Command Prompt"，
REM 或者 ARM64 机器上开 "ARM64 Native Tools Command Prompt" —— 本项目没有任何
REM 架构相关的代码，换个开发者命令行就是原生 ARM64 版本。

for /f "tokens=1,2,3" %%A in ('findstr /B /C:"VERSION" Makefile') do if "%%A"=="VERSION" set "SP_VERSION=%%C"
if not defined SP_VERSION (echo VERSION NOT FOUND IN MAKEFILE & exit /b 1)
set "SP_VERSION_COMMA=%SP_VERSION:.=,%"

if defined VSCMD_ARG_TGT_ARCH (
  set "SP_BUILD_DIR=build\msvc-%VSCMD_ARG_TGT_ARCH%"
) else (
  set "SP_BUILD_DIR=build\msvc-%PROCESSOR_ARCHITECTURE%"
)
if not exist "%SP_BUILD_DIR%" mkdir "%SP_BUILD_DIR%"

rc /nologo /d SP_VER_STR=%SP_VERSION% /d SP_VER_NUM=%SP_VERSION_COMMA%,0 ^
   /fo "%SP_BUILD_DIR%\StarPaper.res" res\StarPaper.rc
if errorlevel 1 (echo RC FAILED & exit /b 1)

cl /nologo /std:c++17 /O2 /EHsc /DUNICODE /D_UNICODE /MT ^
   src\desktop.cpp src\player.cpp src\pipeline.cpp src\theme.cpp ^
   src\widgets.cpp src\thumbs.cpp src\settings.cpp src\startup.cpp src\main.cpp ^
   /Fo:"%SP_BUILD_DIR%\" ^
   /Fe:StarPaper.exe ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup "%SP_BUILD_DIR%\StarPaper.res" ^
   d3d11.lib dxgi.lib mfplat.lib mfuuid.lib ole32.lib oleaut32.lib shell32.lib ^
   comdlg32.lib wtsapi32.lib user32.lib gdi32.lib advapi32.lib comctl32.lib ^
   runtimeobject.lib

if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo.
echo Done -^> StarPaper.exe  (version %SP_VERSION%; intermediates in %SP_BUILD_DIR%)
