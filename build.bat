@echo off
setlocal

set CMAKE="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

echo Building UEVR (Release)...
%CMAKE% --build build --config Release --target uevr
if %ERRORLEVEL% neq 0 (
    echo BUILD FAILED
    exit /b %ERRORLEVEL%
)

echo.
echo Build succeeded. Deploying...
bash deploy.sh
