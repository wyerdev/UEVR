@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   UEVR VR Post-Processing Shaders Installer
echo ============================================
echo.

:: Determine where we are running from
set "SCRIPT_DIR=%~dp0"

:: Target directories
set "UEVR_DATA=%APPDATA%\UnrealVRMod"
set "PLUGIN_DST=%UEVR_DATA%\UEVR\plugins"
set "PRESET_DST=%UEVR_DATA%\UEVR\data\plugins\shipping_presets"

:: Check if plugins subfolder exists (release zip layout)
if exist "%SCRIPT_DIR%plugins\" (
    set "PLUGIN_SRC=%SCRIPT_DIR%plugins"
) else (
    :: Maybe running from the build output directly
    if exist "%SCRIPT_DIR%build\Release\01_LevelsPlusShader.dll" (
        set "PLUGIN_SRC=%SCRIPT_DIR%build\Release"
    ) else (
        echo ERROR: Cannot find shader DLLs.
        echo Expected a "plugins" folder next to this script, or build\Release.
        echo.
        goto :fail
    )
)

:: Check if presets subfolder exists
set "PRESET_SRC="
if exist "%SCRIPT_DIR%shipping_presets\" (
    set "PRESET_SRC=%SCRIPT_DIR%shipping_presets"
) else if exist "%SCRIPT_DIR%presets\" (
    set "PRESET_SRC=%SCRIPT_DIR%presets"
)

:: Count what we have
set "DLL_COUNT=0"
for /f "delims=" %%f in ('dir /b "%PLUGIN_SRC%\*Shader.dll" 2^>nul ^| findstr /r "^[0-9]"') do set /a DLL_COUNT+=1

if %DLL_COUNT%==0 (
    echo ERROR: No shader DLLs found in "%PLUGIN_SRC%".
    echo.
    goto :fail
)

echo Found %DLL_COUNT% shader DLLs to install.
echo.
echo Install location (global - all games):
echo   %PLUGIN_DST%
echo.
echo Presets location:
echo   %PRESET_DST%
echo.

:: Create target directories
if not exist "%PLUGIN_DST%" (
    mkdir "%PLUGIN_DST%"
    if errorlevel 1 (
        echo ERROR: Could not create plugin directory.
        goto :fail
    )
)

:: Copy shader DLLs (only numbered ones like 01_LevelsPlusShader.dll)
set "COPIED=0"
set "ERRORS=0"
echo Installing shaders...
for /f "delims=" %%f in ('dir /b "%PLUGIN_SRC%\*Shader.dll" 2^>nul ^| findstr /r "^[0-9]"') do (
    copy /Y "%PLUGIN_SRC%\%%f" "%PLUGIN_DST%\" >nul
    if errorlevel 1 (
        echo   FAILED: %%f
        set /a ERRORS+=1
    ) else (
        echo   OK: %%f
        set /a COPIED+=1
    )
)

:: Copy license files
for %%f in ("%PLUGIN_SRC%\*-LICENSE.txt") do (
    copy /Y "%%f" "%PLUGIN_DST%\" >nul
    if errorlevel 1 (
        echo   FAILED: %%~nxf
        set /a ERRORS+=1
    ) else (
        echo   OK: %%~nxf
        set /a COPIED+=1
    )
)

:: Copy presets
if defined PRESET_SRC (
    echo.
    echo Installing built-in presets...
    if not exist "%PRESET_DST%" mkdir "%PRESET_DST%"
    for /d %%d in ("%PRESET_SRC%\*") do (
        set "PNAME=%%~nxd"
        if not exist "%PRESET_DST%\!PNAME!" mkdir "%PRESET_DST%\!PNAME!"
        for %%f in ("%%d\*") do (
            copy /Y "%%f" "%PRESET_DST%\!PNAME!\" >nul 2>&1
            if errorlevel 1 (
                set /a ERRORS+=1
            )
        )
        echo   OK: !PNAME!
        set /a COPIED+=1
    )
)

echo.
echo ============================================
if !ERRORS! GTR 0 (
    echo   DONE with !ERRORS! error[s]. !COPIED! items installed.
    echo   Some files may be in use. Close UEVR and retry.
) else (
    echo   SUCCESS: !COPIED! items installed.
)
echo ============================================
echo.
echo Plugins installed to: %PLUGIN_DST%
if defined PRESET_SRC echo Presets installed to:  %PRESET_DST%
echo.
echo NOTE: These shaders require the patched UEVR fork.
echo       They will NOT load on stock UEVR nightly.
echo.
pause
exit /b 0

:fail
echo.
echo ============================================
echo   INSTALLATION FAILED
echo ============================================
echo.
pause
exit /b 1
