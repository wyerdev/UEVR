@echo off
setlocal

:: Find cmake: prefer PATH, fall back to local VS install
where cmake >nul 2>&1
if %ERRORLEVEL% equ 0 (
    set "CMAKE=cmake"
) else (
    set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

echo Cleaning build...
"%CMAKE%" --build build --config Release -- /t:Clean
if %ERRORLEVEL% neq 0 (
    echo CLEAN FAILED
    exit /b %ERRORLEVEL%
)

echo Clean succeeded. Run build.bat to rebuild...