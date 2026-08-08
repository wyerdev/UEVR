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
:: [fork] all build lines load the same shared shader directory.
set "PLUGIN_ROOT=%UEVR_DATA%\UEVR\plugins"
set "PLUGIN_DST=%PLUGIN_ROOT%\shaders"
set "PRESET_DST=%UEVR_DATA%\UEVR\data\plugins\shipping_presets"
set "ASSET_DST=%UEVR_DATA%\UEVR\data\plugins\shader_assets"

:: Since the release zip layout uses pre-indexed "NN_*Shader.dll" filenames,
:: we want local installation to behave exactly the same way to ensure the
:: user's runtime and settings registry are tested against the actual release layout.
:: To do this, we always stage the local DLLs first, assign sequential NN_ prefixes
:: from render_order(), and then perform the install using those prefixed files.
::
:: Two supported layouts:
::   1) Release-zip layout: a `plugins\shaders\` folder next to this script
::      containing already-prefixed `NN_*Shader.dll` files. No Python required;
::      we just install immediately.
::   2) Dev / local-build layout: this script is at the repo root and bare-named
::      `*Shader.dll` files live in the branch-specific build dir. We stage them
::      into a temp directory, run the Python script to assign the NN_ prefixes
::      and copy LICENSE files, and then install from that staged folder.
:: [fork] must match BUILDDIR in build.bat: each branch builds out-of-source
:: into its own directory so the variants never share a cache or objects.
if defined UEVR_BUILD_DIR (
    set "BUILDDIR=%UEVR_BUILD_DIR%"
) else (
    set "BUILDDIR=A:\UEVR-build\afw"
)
if defined UEVR_SHADER_BUILD_DIR (
    set "SHADER_BUILDDIR=%UEVR_SHADER_BUILD_DIR%"
) else (
    set "SHADER_BUILDDIR=A:\UEVR-build\reshade"
)
set "STAGE_TMP="
if exist "%SCRIPT_DIR%plugins\shaders\" (
    set "PLUGIN_SRC=%SCRIPT_DIR%plugins\shaders"
) else if exist "%SHADER_BUILDDIR%\Release\LevelsPlusShader.dll" (
    rem Dev mode: need Python to stage with correct NN_ prefixes.
    where python >nul 2>&1
    if errorlevel 1 (
        echo ERROR: Shared shader build detected at %SHADER_BUILDDIR%\Release\ but Python is not on PATH.
        echo Python is required to assign NN_ shader prefixes from render_order^(^).
        echo Install Python or run via deploy.sh.
        echo.
        goto :fail
    )
    set "STAGE_TMP=%TEMP%\uevr-install-stage-%RANDOM%%RANDOM%"
    mkdir "!STAGE_TMP!" >nul 2>&1
    if errorlevel 1 (
        echo ERROR: Could not create staging dir !STAGE_TMP!
        goto :fail
    )
    echo Staging bare-named DLLs from %SHADER_BUILDDIR%\Release\ to !STAGE_TMP! ...
    copy /Y "%SHADER_BUILDDIR%\Release\*Shader.dll" "!STAGE_TMP!\" >nul
    echo Assigning NN_ prefixes from render_order^(^) ...
    python "%SCRIPT_DIR%scripts\assign_shader_order.py" "!STAGE_TMP!" --exclude Bloom --copy-licenses
    if errorlevel 1 (
        echo ERROR: assign_shader_order.py failed.
        rmdir /s /q "!STAGE_TMP!" >nul 2>&1
        goto :fail
    )
    set "PLUGIN_SRC=!STAGE_TMP!"
) else (
    echo ERROR: Cannot find shader DLLs.
    echo Expected one of:
    echo   - a "plugins\shaders" folder next to this script ^(release-zip layout^), or
    echo   - %SHADER_BUILDDIR%\Release\*Shader.dll ^(shared dev build layout, requires Python^).
    echo.
    goto :fail
)

:: Check if presets subfolder exists (release zip uses shipping_presets/,
:: dev tree uses presets/).
set "PRESET_SRC="
if exist "%SCRIPT_DIR%shipping_presets\" (
    set "PRESET_SRC=%SCRIPT_DIR%shipping_presets"
) else if exist "%SCRIPT_DIR%presets\" (
    set "PRESET_SRC=%SCRIPT_DIR%presets"
)

:: Check for shipped shader assets (LUTs, etc.). Release zip ships these
:: flat in `shader_assets/`. Dev tree has them split per-plugin under
:: `examples/<plugin>/assets/` -- handled separately below.
set "ASSET_SRC="
if exist "%SCRIPT_DIR%shader_assets\" (
    set "ASSET_SRC=%SCRIPT_DIR%shader_assets"
)

:: Count shader DLLs (shared shader paths accept both prefixed and bare names).
set "DLL_COUNT=0"
for /f "delims=" %%f in ('dir /b "%PLUGIN_SRC%\*Shader.dll" 2^>nul') do set /a DLL_COUNT+=1

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

:: Remove old shader files from the unqualified global path and old variant dirs.
call :cleanup_shader_dir "%PLUGIN_ROOT%"
for /d %%v in ("%PLUGIN_ROOT%\*") do (
    if /i not "%%~nxv"=="shaders" call :cleanup_shader_dir "%%~fv"
)
for /d %%g in ("%UEVR_DATA%\*") do (
    if /i not "%%~nxg"=="UEVR" (
        call :cleanup_shader_dir "%%~fg\plugins"
        for /d %%v in ("%%~fg\plugins\*") do call :cleanup_shader_dir "%%~fv"
    )
)

:: Create target directories
if not exist "%PLUGIN_DST%" (
    mkdir "%PLUGIN_DST%"
    if errorlevel 1 (
        echo ERROR: Could not create plugin directory.
        goto :fail
    )
)

:: Copy shader DLLs and LICENSE files
set "COPIED=0"
set "ERRORS=0"

:: Cleanup: remove any previously-installed shared shader DLLs and licenses.
echo Cleaning up any previous shader installation...
for /f "delims=" %%f in ('dir /b "%PLUGIN_DST%\*Shader.dll" 2^>nul') do del /f "%PLUGIN_DST%\%%f" >nul 2>&1
for /f "delims=" %%f in ('dir /b "%PLUGIN_DST%\*Shader-LICENSE.txt" 2^>nul') do del /f "%PLUGIN_DST%\%%f" >nul 2>&1

echo Installing shaders...
for /f "delims=" %%f in ('dir /b "%PLUGIN_SRC%\*Shader.dll" 2^>nul') do (
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
for /f "delims=" %%f in ('dir /b "%PLUGIN_SRC%\*Shader-LICENSE.txt" 2^>nul') do (
    copy /Y "%PLUGIN_SRC%\%%f" "%PLUGIN_DST%\" >nul
    if errorlevel 1 (
        echo   FAILED: %%f
        set /a ERRORS+=1
    ) else (
        echo   OK: %%f
        set /a COPIED+=1
    )
)

:: Copy presets
if defined PRESET_SRC (
    echo.
    echo Installing built-in presets...
    rem Wipe stale entries (legacy folder-style and any removed *.uevrpreset)
    rem so renames/deletions in the package are reflected on disk.
    rem NOTE: do NOT use :: comments inside a (...) block; a literal ) in the
    rem comment text closes the block early and breaks parsing.
    if exist "%PRESET_DST%" rmdir /s /q "%PRESET_DST%"
    mkdir "%PRESET_DST%"
    for %%f in ("%PRESET_SRC%\*.uevrpreset") do (
        copy /Y "%%f" "%PRESET_DST%\" >nul
        if errorlevel 1 (
            echo   FAILED: %%~nxf
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
    for %%f in ("%ASSET_SRC%\*") do (
        copy /Y "%%f" "%ASSET_DST%\" >nul
        if errorlevel 1 (
            echo   FAILED: %%~nxf
            set /a ERRORS+=1
        ) else (
            echo   OK: %%~nxf
            set /a COPIED+=1
        )
    )
) else if exist "%SCRIPT_DIR%examples\" (
    rem Dev mode: assets live per-plugin under examples\<plugin>\assets\.
    echo.
    echo Installing shader assets ^(dev layout^)...
    if not exist "%ASSET_DST%" mkdir "%ASSET_DST%"
    for /d %%d in ("%SCRIPT_DIR%examples\*") do (
        if exist "%%d\assets\" (
            for %%f in ("%%d\assets\*") do (
                copy /Y "%%f" "%ASSET_DST%\" >nul
                if errorlevel 1 (
                    echo   FAILED: %%~nxf
                    set /a ERRORS+=1
                ) else (
                    echo   OK: %%~nxf
                    set /a COPIED+=1
                )
            )
        )
    )
)

:: Copy uninstall script to plugins folder so users can run it from there
if exist "%SCRIPT_DIR%uninstall-plugins.bat" (
    copy /Y "%SCRIPT_DIR%uninstall-plugins.bat" "%PLUGIN_DST%\" >nul
    if errorlevel 1 (
        echo.
        echo WARNING: Could not copy uninstaller to %PLUGIN_DST%\
        set /a ERRORS+=1
    ) else (
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
:: Clean up dev-mode staging dir if we created one.
if defined STAGE_TMP if exist "%STAGE_TMP%" rmdir /s /q "%STAGE_TMP%" >nul 2>&1
pause
if !ERRORS! GTR 0 exit /b 1
exit /b 0

:fail
echo.
echo ============================================
echo   INSTALLATION FAILED
echo ============================================
echo.
if defined STAGE_TMP if exist "%STAGE_TMP%" rmdir /s /q "%STAGE_TMP%" >nul 2>&1
pause
exit /b 1

:cleanup_shader_dir
set "CLEANUP_DIR=%~1"
for /f "delims=" %%f in ('dir /b "%CLEANUP_DIR%\*Shader.dll" 2^>nul') do del /f "%CLEANUP_DIR%\%%f" >nul 2>&1
for /f "delims=" %%f in ('dir /b "%CLEANUP_DIR%\*Shader-LICENSE.txt" 2^>nul') do del /f "%CLEANUP_DIR%\%%f" >nul 2>&1
exit /b 0
