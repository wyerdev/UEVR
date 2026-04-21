@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   UEVR VR Post-Processing Shaders Uninstaller
echo ============================================
echo.

set "UEVR_DATA=%APPDATA%\UnrealVRMod"
set "PLUGIN_DIR=%UEVR_DATA%\UEVR\plugins"
set "PRESET_DIR=%UEVR_DATA%\UEVR\data\plugins\shipping_presets"
set "USER_PRESET_DIR=%UEVR_DATA%\UEVR\data\plugins\presets"

:: Detect if we're running from inside the plugins folder already
set "SCRIPT_DIR=%~dp0"
if /i "%SCRIPT_DIR:~0,-1%"=="%PLUGIN_DIR%" (
    echo Detected: running from plugins folder.
) else (
    echo Plugin folder: %PLUGIN_DIR%
)

:: List of plugin DLLs to remove (our 16 post-processing plugins)
set "PLUGINS=01_LevelsPlusShader.dll 02_LiftGammaGainShader.dll 03_TonemapShader.dll 04_CurvesShader.dll 05_FakeHDRShader.dll 06_DPXShader.dll 07_TechnicolorShader.dll 08_ColourfulnessShader.dll 09_VibranceShader.dll 10_FilmGrain2Shader.dll 11_HSLShiftShader.dll 12_FilmicPassShader.dll 13_ClarityShader.dll 14_CASShader.dll 15_LumaSharpenShader.dll 16_DebandShader.dll"

:: Corresponding license files
set "LICENSES=01_LevelsPlusShader-LICENSE.txt 02_LiftGammaGainShader-LICENSE.txt 03_TonemapShader-LICENSE.txt 04_CurvesShader-LICENSE.txt 05_FakeHDRShader-LICENSE.txt 06_DPXShader-LICENSE.txt 07_TechnicolorShader-LICENSE.txt 08_ColourfulnessShader-LICENSE.txt 09_VibranceShader-LICENSE.txt 10_FilmGrain2Shader-LICENSE.txt 11_HSLShiftShader-LICENSE.txt 12_FilmicPassShader-LICENSE.txt 13_ClarityShader-LICENSE.txt 14_CASShader-LICENSE.txt 15_LumaSharpenShader-LICENSE.txt 16_DebandShader-LICENSE.txt"

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
echo   - All 16 post-processing shader DLLs from global plugins
echo   - Their license files
echo   - Built-in shipping presets
echo   - User-saved presets
echo   - Per-game shader settings
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
        del /f "%PLUGIN_DIR%\%%f" 2>&1
        if exist "%PLUGIN_DIR%\%%f" (
            echo   FAILED: %%f
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
        del /f "%PLUGIN_DIR%\%%f" 2>&1
        if exist "%PLUGIN_DIR%\%%f" (
            echo   FAILED: %%f
            set /a ERRORS+=1
        ) else (
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

:: Remove user-saved presets
if exist "%USER_PRESET_DIR%" (
    echo.
    echo Removing user presets...
    rmdir /s /q "%USER_PRESET_DIR%" >nul 2>&1
    if errorlevel 1 (
        echo   FAILED: Could not remove user presets folder.
        set /a ERRORS+=1
    ) else (
        echo   Removed: user presets
        set /a REMOVED+=1
    )
)

:: Our 16 shader settings filenames (must match PluginLoader name derivation)
set "SETTINGS=levelsplus_settings.txt liftgammagain_settings.txt tonemap_settings.txt curves_settings.txt fakehdr_settings.txt dpx_settings.txt technicolor_settings.txt colourfulness_settings.txt vibrance_settings.txt filmgrain2_settings.txt hslshift_settings.txt filmicpass_settings.txt clarity_settings.txt cas_settings.txt lumasharpen_settings.txt deband_settings.txt"

echo.
echo Removing per-game shader settings...
for /d %%g in ("%UEVR_DATA%\*") do (
    if exist "%%g\data\plugins" (
        :: Clean new location (shader_settings subdir)
        if exist "%%g\data\plugins\shader_settings" (
            rmdir /s /q "%%g\data\plugins\shader_settings" >nul 2>&1
            if not errorlevel 1 set /a REMOVED+=1
        )
        :: Clean old location (pre-migration files in data\plugins directly)
        for %%s in (%SETTINGS%) do (
            if exist "%%g\data\plugins\%%s" (
                del /f "%%g\data\plugins\%%s" >nul 2>&1
                if not errorlevel 1 set /a REMOVED+=1
            )
        )
        if exist "%%g\data\plugins\active_preset.txt" (
            del /f "%%g\data\plugins\active_preset.txt" >nul 2>&1
            if not errorlevel 1 set /a REMOVED+=1
        )
        :: Remove per-game presets
        if exist "%%g\data\plugins\presets" (
            rmdir /s /q "%%g\data\plugins\presets" >nul 2>&1
            if not errorlevel 1 set /a REMOVED+=1
        )
    )
)

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
exit /b 0
