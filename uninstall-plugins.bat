@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   UEVR VR Post-Processing Shaders Uninstaller
echo   Variant: reshade-afw
echo ============================================
echo.

set "UEVR_DATA=%APPDATA%\UnrealVRMod"
:: [fork] variant-isolation: must match UEVR_VARIANT_ID in include\uevr\Variant.hpp.
:: Every path below is qualified by %VARIANT%, so this uninstaller can never
:: touch another build variant's DLLs, presets, assets or per-game settings.
set "VARIANT=reshade-afw"
set "PLUGIN_DIR=%UEVR_DATA%\UEVR\plugins\%VARIANT%"
set "PRESET_DIR=%UEVR_DATA%\UEVR\data\plugins\%VARIANT%\shipping_presets"
set "USER_PRESET_DIR=%UEVR_DATA%\UEVR\data\plugins\%VARIANT%\presets"
set "ASSET_DIR=%UEVR_DATA%\UEVR\data\plugins\%VARIANT%\shader_assets"

:: Detect if we're running from inside the plugins folder already
set "SCRIPT_DIR=%~dp0"
if /i "%SCRIPT_DIR:~0,-1%"=="%PLUGIN_DIR%" (
    echo Detected: running from plugins folder.
) else (
    echo Plugin folder: %PLUGIN_DIR%
)

:: Check if anything is installed
set "FOUND=0"
for /f "delims=" %%f in ('dir /b "%PLUGIN_DIR%\*Shader.dll" 2^>nul ^| findstr /r "^[0-9]"') do set /a FOUND+=1

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
echo This will remove, for variant "%VARIANT%" only:
echo   - All post-processing shader DLLs from global plugins
echo   - Their license files
echo   - Built-in shipping presets
echo   - User-saved presets
echo   - Per-game shader settings
echo.
echo Other UEVR build variants are left untouched.
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

:: Remove plugin DLLs and matching LICENSE files. Match pattern:
:: "<digits>_*Shader.dll" (and the matching LICENSE.txt). All shaders we ship
:: are installed as NN_<Name>Shader.dll (the NN_ prefix is auto-assigned by
:: scripts/assign_shader_order.py from each plugin's render_order()). This
:: pattern is unique to our releases — any third-party plugin DLL or any file
:: a user dropped here manually will NOT have a leading-digit prefix and will
:: not be touched. This also handles every past / future shader rename or
:: removal without needing to track a list of historical names: the glob
:: catches all of them by shape alone.
echo Removing shaders...
for /f "delims=" %%f in ('dir /b "%PLUGIN_DIR%\*Shader.dll" 2^>nul ^| findstr /r "^[0-9]"') do (
    del /f "%PLUGIN_DIR%\%%f" 2>&1
    if exist "%PLUGIN_DIR%\%%f" (
        echo   FAILED: %%f
        set /a ERRORS+=1
    ) else (
        echo   Removed: %%f
        set /a REMOVED+=1
    )
)
for /f "delims=" %%f in ('dir /b "%PLUGIN_DIR%\*Shader-LICENSE.txt" 2^>nul ^| findstr /r "^[0-9]"') do (
    del /f "%PLUGIN_DIR%\%%f" 2>&1
    if not exist "%PLUGIN_DIR%\%%f" set /a REMOVED+=1
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

:: Per-game data is variant-scoped: everything this build ever wrote lives
:: under <game>\data\plugins\%VARIANT%\, so the whole directory can go.
::
:: Legacy unqualified files (pre-variant: <game>\data\plugins\*_settings.txt,
:: \shader_settings, \presets, \active_preset.txt) are deliberately NOT touched.
:: They carry no variant attribution, so they may belong to another build.
:: The backend migrates whatever it finds there into its own variant dir on
:: first launch; after that, this uninstaller owns it.

echo.
echo Removing per-game shader settings...
for /d %%g in ("%UEVR_DATA%\*") do (
    if exist "%%g\data\plugins\%VARIANT%" (
        rmdir /s /q "%%g\data\plugins\%VARIANT%" >nul 2>&1
        if not errorlevel 1 set /a REMOVED+=1
    )
)

:: Drop the now-empty variant plugin dir so no stale folder is left behind.
rmdir "%PLUGIN_DIR%" >nul 2>&1
rmdir "%UEVR_DATA%\UEVR\data\plugins\%VARIANT%" >nul 2>&1

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
