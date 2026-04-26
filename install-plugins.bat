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
set "ASSET_DST=%UEVR_DATA%\UEVR\data\plugins\shader_assets"

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

:: Check for shipped shader assets (LUTs, etc.). Release zip layout uses
:: a flat `shader_assets` folder; dev layout uses `examples/<plugin>/assets/`.
set "ASSET_SRC="
if exist "%SCRIPT_DIR%shader_assets\" (
    set "ASSET_SRC=%SCRIPT_DIR%shader_assets"
) else if exist "%SCRIPT_DIR%examples\" (
    set "ASSET_SRC=%SCRIPT_DIR%examples"
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

:: Clean up any old-prefix copies of our shaders first.
:: Plugin numeric prefixes change between releases (e.g. BlackCrush moved from 17 to 05,
:: shifting FakeHDR/Deband/etc. by one). Without this cleanup, both the old and new
:: prefix DLLs coexist in PLUGIN_DST and the same shader runs twice per frame.
:: We match by the unique suffix so any past or future numbering is handled.
set "SHADER_SUFFIXES=LevelsPlusShader LiftGammaGainShader TonemapShader CurvesShader BlackCrushShader FakeHDRShader DPXShader TechnicolorShader ColourfulnessShader VibranceShader FilmGrain2Shader HSLShiftShader FilmicPassShader ClarityShader CASShader LumaSharpenShader DebandShader LUTShader BloomShader AdaptiveTonemapperShader"
echo Cleaning up any previous shader installation...
for %%s in (%SHADER_SUFFIXES%) do (
    for %%f in ("%PLUGIN_DST%\*%%s.dll") do if exist "%%f" del /f "%%f" >nul 2>&1
    for %%f in ("%PLUGIN_DST%\*%%s-LICENSE.txt") do if exist "%%f" del /f "%%f" >nul 2>&1
)

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
    :: Wipe stale entries (legacy folder-style and any *.uevrpreset removed
    :: upstream) so renames/deletions in the package are reflected on disk.
    if exist "%PRESET_DST%" rmdir /s /q "%PRESET_DST%"
    mkdir "%PRESET_DST%"
    for %%f in ("%PRESET_SRC%\*.uevrpreset") do (
        copy /Y "%%f" "%PRESET_DST%\" >nul 2>&1
        if errorlevel 1 (
            set /a ERRORS+=1
        ) else (
            echo   OK: %%~nxf
            set /a COPIED+=1
        )
    )
)

:: Copy shipped shader assets (LUTs, textures, etc.).
:: Plugins resolve via plugin_settings.hpp's resolve_shader_asset_path():
::   1. <persistent>/data/plugins/shader_settings/<file> (per-game user override)
::   2. <UEVR_root>/data/plugins/shader_assets/<file>    (this directory; shipped fallback)
if defined ASSET_SRC (
    echo.
    echo Installing shader assets...
    if not exist "%ASSET_DST%" mkdir "%ASSET_DST%"
    if exist "%SCRIPT_DIR%shader_assets\" (
        for %%f in ("%ASSET_SRC%\*") do (
            copy /Y "%%f" "%ASSET_DST%\" >nul 2>&1
            if not errorlevel 1 (
                echo   OK: %%~nxf
                set /a COPIED+=1
            )
        )
    ) else (
        for /d %%d in ("%ASSET_SRC%\*") do (
            if exist "%%d\assets\" (
                for %%f in ("%%d\assets\*") do (
                    copy /Y "%%f" "%ASSET_DST%\" >nul 2>&1
                    if not errorlevel 1 (
                        echo   OK: %%~nxf
                        set /a COPIED+=1
                    )
                )
            )
        )
    )
)

:: Copy uninstall script to plugins folder so users can run it from there
if exist "%SCRIPT_DIR%uninstall-plugins.bat" (
    copy /Y "%SCRIPT_DIR%uninstall-plugins.bat" "%PLUGIN_DST%\" >nul 2>&1
    if not errorlevel 1 (
        echo.
        echo Uninstaller copied to: %PLUGIN_DST%\uninstall-plugins.bat
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
