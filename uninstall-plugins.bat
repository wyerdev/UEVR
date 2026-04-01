@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   UEVR VR Post-Processing Plugins Uninstaller
echo ============================================
echo.

set "UEVR_DATA=%APPDATA%\UnrealVRMod"
set "PLUGIN_DIR=%UEVR_DATA%\UEVR\plugins"
set "PRESET_DIR=%UEVR_DATA%\UEVR\data\plugins\shipping_presets"

:: Detect if we're running from inside the plugins folder already
set "SCRIPT_DIR=%~dp0"
if /i "%SCRIPT_DIR:~0,-1%"=="%PLUGIN_DIR%" (
    echo Detected: running from plugins folder.
) else (
    echo Plugin folder: %PLUGIN_DIR%
)

:: List of plugin DLLs to remove (our 13 post-processing plugins)
set "PLUGINS=01_LevelsPlusPlugin.dll 02_LiftGammaGainPlugin.dll 03_TonemapPlugin.dll 04_CurvesPlugin.dll 05_FakeHDRPlugin.dll 06_DPXPlugin.dll 07_TechnicolorPlugin.dll 08_ColourfulnessPlugin.dll 09_VibrancePlugin.dll 10_FilmGrain2Plugin.dll 11_HSLShiftPlugin.dll 12_FilmicPassPlugin.dll 13_ClarityPlugin.dll"

:: Corresponding license files
set "LICENSES=01_LevelsPlusPlugin-LICENSE.txt 02_LiftGammaGainPlugin-LICENSE.txt 03_TonemapPlugin-LICENSE.txt 04_CurvesPlugin-LICENSE.txt 05_FakeHDRPlugin-LICENSE.txt 06_DPXPlugin-LICENSE.txt 07_TechnicolorPlugin-LICENSE.txt 08_ColourfulnessPlugin-LICENSE.txt 09_VibrancePlugin-LICENSE.txt 10_FilmGrain2Plugin-LICENSE.txt 11_HSLShiftPlugin-LICENSE.txt 12_FilmicPassPlugin-LICENSE.txt 13_ClarityPlugin-LICENSE.txt"

:: Check if anything is installed
set "FOUND=0"
for %%f in (%PLUGINS%) do (
    if exist "%PLUGIN_DIR%\%%f" set /a FOUND+=1
)

if %FOUND%==0 (
    echo No post-processing plugins found in:
    echo   %PLUGIN_DIR%
    echo.
    echo Nothing to uninstall.
    echo.
    pause
    exit /b 0
)

echo Found %FOUND% plugin(s) to remove.
echo.
echo This will remove:
echo   - All 13 post-processing plugin DLLs from global plugins
echo   - Their license files
echo   - Built-in shipping presets
echo.
echo Per-game settings and user-saved presets will NOT be removed.
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

:: Remove plugin DLLs
echo Removing plugins...
for %%f in (%PLUGINS%) do (
    if exist "%PLUGIN_DIR%\%%f" (
        del /f "%PLUGIN_DIR%\%%f" >nul 2>&1
        if errorlevel 1 (
            echo   FAILED: %%f  (may be in use - close UEVR first)
            set /a ERRORS+=1
        ) else (
            echo   Removed: %%f
            set /a REMOVED+=1
        )
    )
)

:: Remove license files
for %%f in (%LICENSES%) do (
    if exist "%PLUGIN_DIR%\%%f" (
        del /f "%PLUGIN_DIR%\%%f" >nul 2>&1
        if not errorlevel 1 (
            set /a REMOVED+=1
        )
    )
)

:: Remove shipping presets
if exist "%PRESET_DIR%" (
    echo.
    echo Removing built-in presets...
    rmdir /s /q "%PRESET_DIR%" >nul 2>&1
    if errorlevel 1 (
        echo   FAILED: Could not remove shipping_presets folder.
        set /a ERRORS+=1
    ) else (
        echo   Removed: shipping_presets
        set /a REMOVED+=1
    )
)

echo.
echo ============================================
if !ERRORS! GTR 0 (
    echo   DONE with !ERRORS! error(s). !REMOVED! items removed.
    echo   Close UEVR and retry for locked files.
) else (
    echo   SUCCESS: !REMOVED! items removed.
)
echo ============================================
echo.
pause
exit /b 0
