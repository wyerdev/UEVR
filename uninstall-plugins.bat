@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   UEVR VR Post-Processing Shaders Uninstaller
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
set "PLUGINS=01_LevelsPlusShader.dll 02_LiftGammaGainShader.dll 03_TonemapShader.dll 04_CurvesShader.dll 05_FakeHDRShader.dll 06_DPXShader.dll 07_TechnicolorShader.dll 08_ColourfulnessShader.dll 09_VibranceShader.dll 10_FilmGrain2Shader.dll 11_HSLShiftShader.dll 12_FilmicPassShader.dll 13_ClarityShader.dll"

:: Corresponding license files
set "LICENSES=01_LevelsPlusShader-LICENSE.txt 02_LiftGammaGainShader-LICENSE.txt 03_TonemapShader-LICENSE.txt 04_CurvesShader-LICENSE.txt 05_FakeHDRShader-LICENSE.txt 06_DPXShader-LICENSE.txt 07_TechnicolorShader-LICENSE.txt 08_ColourfulnessShader-LICENSE.txt 09_VibranceShader-LICENSE.txt 10_FilmGrain2Shader-LICENSE.txt 11_HSLShiftShader-LICENSE.txt 12_FilmicPassShader-LICENSE.txt 13_ClarityShader-LICENSE.txt"

:: Check if anything is installed
set "FOUND=0"
for %%f in (%PLUGINS%) do (
    if exist "%PLUGIN_DIR%\%%f" set /a FOUND+=1
)

if %FOUND%==0 (
    echo No post-processing shaders found in:
    echo   %PLUGIN_DIR%
    echo.
    echo Nothing to uninstall.
    echo.
    pause
    exit /b 0
)

echo Found %FOUND% shader(s) to remove.
echo.
echo This will remove:
echo   - All 13 post-processing shader DLLs from global plugins
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
echo Removing shaders...
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
