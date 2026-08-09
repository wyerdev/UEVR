@echo off
setlocal

:: [fork] must match BUILDDIR in build.bat: each branch builds out-of-source
:: into its own directory so the variants never share a cache or objects.
if defined UEVR_BUILD_DIR (
    set "BUILDDIR=%UEVR_BUILD_DIR%"
) else (
    set "BUILDDIR=A:\UEVR-build\afw-joeyhodge"
)

:: Find cmake: prefer PATH, fall back to local VS install
where cmake >nul 2>&1
if %ERRORLEVEL% equ 0 (
    set "CMAKE=cmake"
) else (
    set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if not exist "%BUILDDIR%\CMakeCache.txt" (
    echo No build at %BUILDDIR%; nothing to clean.
    exit /b 0
)

echo Cleaning %BUILDDIR% ...
"%CMAKE%" --build "%BUILDDIR%" --config Release -- /t:Clean
if errorlevel 1 (
    echo CLEAN FAILED
    exit /b 1
)

echo Clean succeeded. Run build.bat to rebuild...