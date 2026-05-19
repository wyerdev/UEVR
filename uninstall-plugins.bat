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
set "ASSET_DIR=%UEVR_DATA%\UEVR\data\plugins\shader_assets"

:: Detect if we're running from inside the plugins folder already
set "SCRIPT_DIR=%~dp0"
if /i "%SCRIPT_DIR:~0,-1%"=="%PLUGIN_DIR%" (
    echo Detected: running from plugins folder.
) else (
    echo Plugin folder: %PLUGIN_DIR%
)

:: List of plugin DLL suffixes to remove (any numeric prefix, current or legacy).
:: Using suffixes makes the uninstaller resilient to renumbering between releases.
set "PLUGINS=FGFXLargeScalePerceptualObscuranceIrradianceShader.dll LSPOIrrShader.dll LevelsPlusShader.dll LiftGammaGainShader.dll TonemapShader.dll CurvesShader.dll BlackCrushShader.dll FakeHDRShader.dll DPXShader.dll TechnicolorShader.dll Technicolor2Shader.dll ColourfulnessShader.dll VibranceShader.dll FilmGrain2Shader.dll HSLShiftShader.dll FilmicPassShader.dll CartoonShader.dll FXAAShader.dll ClarityShader.dll CASShader.dll LumaSharpenShader.dll DebandShader.dll LUTShader.dll BloomShader.dll AdaptiveTonemapperShader.dll EyeAdaptionShader.dll"

:: Corresponding license file suffixes
set "LICENSES=FGFXLargeScalePerceptualObscuranceIrradianceShader-LICENSE.txt LSPOIrrShader-LICENSE.txt LevelsPlusShader-LICENSE.txt LiftGammaGainShader-LICENSE.txt TonemapShader-LICENSE.txt CurvesShader-LICENSE.txt BlackCrushShader-LICENSE.txt FakeHDRShader-LICENSE.txt DPXShader-LICENSE.txt TechnicolorShader-LICENSE.txt Technicolor2Shader-LICENSE.txt ColourfulnessShader-LICENSE.txt VibranceShader-LICENSE.txt FilmGrain2Shader-LICENSE.txt HSLShiftShader-LICENSE.txt FilmicPassShader-LICENSE.txt CartoonShader-LICENSE.txt FXAAShader-LICENSE.txt ClarityShader-LICENSE.txt CASShader-LICENSE.txt LumaSharpenShader-LICENSE.txt DebandShader-LICENSE.txt LUTShader-LICENSE.txt BloomShader-LICENSE.txt AdaptiveTonemapperShader-LICENSE.txt EyeAdaptionShader-LICENSE.txt"

:: Check if anything is installed (match any prefix)
set "FOUND=0"
for %%f in (%PLUGINS%) do (
    for %%g in ("%PLUGIN_DIR%\*%%f") do if exist "%%g" set /a FOUND+=1
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
echo   - All post-processing shader DLLs from global plugins
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

:: Remove plugin DLLs (any numeric prefix)
echo Removing shaders...
for %%f in (%PLUGINS%) do (
    for %%g in ("%PLUGIN_DIR%\*%%f") do (
        if exist "%%g" (
            del /f "%%g" 2>&1
            if exist "%%g" (
                echo   FAILED: %%~nxg
                set /a ERRORS+=1
            ) else (
                echo   Removed: %%~nxg
                set /a REMOVED+=1
            )
        )
    )
)

:: Remove license files (any numeric prefix)
for %%f in (%LICENSES%) do (
    for %%g in ("%PLUGIN_DIR%\*%%f") do (
        if exist "%%g" (
            del /f "%%g" 2>&1
            if exist "%%g" (
                echo   FAILED: %%~nxg
                set /a ERRORS+=1
            ) else (
                set /a REMOVED+=1
            )
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

:: Remove shipped shader assets (LUTs, textures, etc.)
if exist "%ASSET_DIR%" (
    echo.
    echo Removing shader assets...
    rmdir /s /q "%ASSET_DIR%" >nul 2>&1
    if errorlevel 1 (
        echo   FAILED: Could not remove shader_assets folder.
        set /a ERRORS+=1
    ) else (
        echo   Removed: shader_assets
        set /a REMOVED+=1
    )
)

:: Legacy per-plugin shader settings filenames (must match PluginLoader name derivation).
:: Used to clean pre-migration files in data\plugins\ alongside the new
:: data\plugins\shader_settings\ folder.
set "SETTINGS=levelsplus_settings.txt liftgammagain_settings.txt tonemap_settings.txt curves_settings.txt fakehdr_settings.txt dpx_settings.txt technicolor_settings.txt technicolor2_settings.txt colourfulness_settings.txt vibrance_settings.txt filmgrain2_settings.txt hslshift_settings.txt filmicpass_settings.txt fxaa_settings.txt clarity_settings.txt cas_settings.txt lumasharpen_settings.txt deband_settings.txt lut_settings.txt bloom_settings.txt adaptive_tonemapper_settings.txt eyeadaption_settings.txt lspoirr_settings.txt"

echo.
echo Removing per-game shader settings...
for /d %%g in ("%UEVR_DATA%\*") do (
    if exist "%%g\data\plugins" (
        rem Clean new location: shader_settings subdir
        if exist "%%g\data\plugins\shader_settings" (
            rmdir /s /q "%%g\data\plugins\shader_settings" >nul 2>&1
            if not errorlevel 1 set /a REMOVED+=1
        )
        rem Clean old location: pre-migration files in data\plugins directly
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
        rem Remove per-game presets
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
if !ERRORS! GTR 0 exit /b 1
exit /b 0
