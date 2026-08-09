@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   UEVR VR Post-Processing Shaders Uninstaller
echo ============================================
echo.

set "UEVR_DATA=%APPDATA%\UnrealVRMod"

:: [fork] variant-isolation: must match UEVR_VARIANT_ID in include\uevr\Variant.hpp.
:: This is the variant this copy of the script was built for; it is only used as
:: the default choice. The removal logic itself is variant-agnostic, so this one
:: script is identical on every branch except for this line.
:: Legacy (pre-variant) install locations. Releases before variant isolation
:: wrote everything unqualified. Those files are still ours and must be removed
:: too, otherwise an upgraded user keeps loading stale shader DLLs. Only files we
:: can positively identify as ours are touched -- see the name-shape rules at
:: each removal step. Third-party plugins are never matched.
set "PLUGIN_ROOT=%UEVR_DATA%\UEVR\plugins"
set "FOUND=0"

call :has_shader_files "%PLUGIN_ROOT%"
call :has_shader_files "%PLUGIN_ROOT%\shaders"
for /d %%v in ("%PLUGIN_ROOT%\*") do (
    if /i not "%%~nxv"=="shaders" call :has_shader_files "%%~fv"
)
for /d %%g in ("%UEVR_DATA%\*") do (
    if /i not "%%~nxg"=="UEVR" (
        call :has_shader_files "%%~fg\plugins"
        call :has_shader_files "%%~fg\plugins\shaders"
        for /d %%v in ("%%~fg\plugins\*") do (
            if /i not "%%~nxv"=="shaders" call :has_shader_files "%%~fv"
        )
    )
)

if "%FOUND%"=="0" (
    echo No post-processing shader files found.
    echo.
    pause
    exit /b 0
)

echo This removes the shared shader DLL set used by all three UEVR build lines.
echo It also removes this fork's old shader DLLs from global and per-game plugin
echo locations.
echo.
echo User data under data\plugins, including presets, settings and shader assets,
echo is left untouched.
echo.
set /p "CONFIRM=Are you sure? (Y/N): "
if /i not "%CONFIRM%"=="Y" (
    echo Cancelled.
    pause
    exit /b 0
)

echo.
set "REMOVED=0"
set "ERRORS=0"

call :remove_shader_files "%PLUGIN_ROOT%"
call :remove_shader_files "%PLUGIN_ROOT%\shaders"
for /d %%v in ("%PLUGIN_ROOT%\*") do (
    if /i not "%%~nxv"=="shaders" call :remove_shader_files "%%~fv"
)
for /d %%g in ("%UEVR_DATA%\*") do (
    if /i not "%%~nxg"=="UEVR" (
        call :remove_shader_files "%%~fg\plugins"
        call :remove_shader_files "%%~fg\plugins\shaders"
        for /d %%v in ("%%~fg\plugins\*") do (
            if /i not "%%~nxv"=="shaders" call :remove_shader_files "%%~fv"
        )
        call :remove_empty_dir "%%~fg\plugins"
    )
)

call :remove_empty_dir "%PLUGIN_ROOT%\shaders"
for /d %%v in ("%PLUGIN_ROOT%\*") do (
    if /i not "%%~nxv"=="shaders" call :remove_empty_dir "%%~fv"
)
call :remove_empty_dir "%PLUGIN_ROOT%"

echo.
echo ============================================
if !ERRORS! GTR 0 (
    echo   DONE with !ERRORS! error[s]. !REMOVED! items removed.
    echo   Close UEVR and retry for locked files.
) else (
    echo   SUCCESS: !REMOVED! items removed.
)
echo ============================================
echo.
pause
if !ERRORS! GTR 0 exit /b 1
exit /b 0
:has_shader_files
set "CHECK_DIR=%~1"
for /f "delims=" %%f in ('dir /b "%CHECK_DIR%\*Shader.dll" 2^>nul') do set "FOUND=1"
for /f "delims=" %%f in ('dir /b "%CHECK_DIR%\*Shader-LICENSE.txt" 2^>nul') do set "FOUND=1"
if exist "%CHECK_DIR%\uninstall-plugins.bat" set "FOUND=1"
exit /b 0

:remove_shader_files
set "REMOVE_DIR=%~1"
for /f "delims=" %%f in ('dir /b "%REMOVE_DIR%\*Shader.dll" 2^>nul') do (
    del /f "%REMOVE_DIR%\%%f" >nul 2>&1
    if exist "%REMOVE_DIR%\%%f" (
        echo   FAILED: %REMOVE_DIR%\%%f
        set /a ERRORS+=1
    ) else (
        echo   Removed: %REMOVE_DIR%\%%f
        set /a REMOVED+=1
    )
)
for /f "delims=" %%f in ('dir /b "%REMOVE_DIR%\*Shader-LICENSE.txt" 2^>nul') do (
    del /f "%REMOVE_DIR%\%%f" >nul 2>&1
    if exist "%REMOVE_DIR%\%%f" (
        echo   FAILED: %REMOVE_DIR%\%%f
        set /a ERRORS+=1
    ) else (
        echo   Removed: %REMOVE_DIR%\%%f
        set /a REMOVED+=1
    )
)
if exist "%REMOVE_DIR%\uninstall-plugins.bat" (
    del /f "%REMOVE_DIR%\uninstall-plugins.bat" >nul 2>&1
    if exist "%REMOVE_DIR%\uninstall-plugins.bat" (
        echo   FAILED: %REMOVE_DIR%\uninstall-plugins.bat
        set /a ERRORS+=1
    ) else (
        echo   Removed: %REMOVE_DIR%\uninstall-plugins.bat
        set /a REMOVED+=1
    )
)
exit /b 0

:remove_empty_dir
if not exist "%~1" exit /b 0
rmdir "%~1" >nul 2>&1
if exist "%~1" exit /b 0
set /a REMOVED+=1
exit /b 0
goto :eof
