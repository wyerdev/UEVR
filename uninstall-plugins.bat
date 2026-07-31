@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   UEVR VR Post-Processing Shaders Uninstaller
echo   Built for variant: reshade
echo ============================================
echo.

set "UEVR_DATA=%APPDATA%\UnrealVRMod"

:: [fork] variant-isolation: must match UEVR_VARIANT_ID in include\uevr\Variant.hpp.
:: This is the variant this copy of the script was built for; it is only used as
:: the default choice. The removal logic itself is variant-agnostic, so this one
:: script is identical on every branch except for this line.
set "VARIANT=reshade"

:: Legacy (pre-variant) install locations. Releases before variant isolation
:: wrote everything unqualified. Those files are still ours and must be removed
:: too, otherwise an upgraded user keeps loading stale shader DLLs. Only files we
:: can positively identify as ours are touched -- see the name-shape rules at
:: each removal step. Third-party plugins are never matched.
set "PLUGIN_ROOT=%UEVR_DATA%\UEVR\plugins"
set "DATA_ROOT=%UEVR_DATA%\UEVR\data\plugins"

:: --------------------------------------------------------------------------
:: Discover every installed variant.
::
:: A subdirectory of plugins\ counts as one of ours only if it contains at least
:: one *Shader.dll. That suffix is the naming shape every plugin in this suite
:: is built with, so a third-party plugin folder will not match.
::
:: Do NOT additionally require the NN_ prefix here: scripts/assign_shader_order.py
:: excludes some plugins (Bloom) from prefixing, so a prefix-only match silently
:: leaves those DLLs behind.
:: --------------------------------------------------------------------------
set "VARIANT_COUNT=0"
for /d %%v in ("%PLUGIN_ROOT%\*") do (
    set "HAS_SHADERS=0"
    for /f "delims=" %%f in ('dir /b "%%v\*Shader.dll" 2^>nul') do set "HAS_SHADERS=1"
    if "!HAS_SHADERS!"=="1" (
        set /a VARIANT_COUNT+=1
        set "VARIANT_!VARIANT_COUNT!=%%~nxv"
    )
)

:: Legacy unqualified install sitting directly in plugins\.
set "LEGACY_FOUND=0"
for /f "delims=" %%f in ('dir /b "%PLUGIN_ROOT%\*Shader.dll" 2^>nul') do set "LEGACY_FOUND=1"

if %VARIANT_COUNT%==0 if "%LEGACY_FOUND%"=="0" (
    echo No post-processing shaders found in:
    echo   %PLUGIN_ROOT%
    echo.
    echo Nothing to uninstall.
    echo.
    pause
    exit /b 0
)

:: --------------------------------------------------------------------------
:: Choose what to remove.
:: --------------------------------------------------------------------------
set "SELECTED="

if %VARIANT_COUNT% LEQ 1 (
    if %VARIANT_COUNT%==1 set "SELECTED=!VARIANT_1!"
) else (
    echo Multiple UEVR build variants are installed:
    echo.
    for /l %%i in (1,1,%VARIANT_COUNT%) do (
        if /i "!VARIANT_%%i!"=="%VARIANT%" (
            echo    [%%i] !VARIANT_%%i!  ^(this build^)
        ) else (
            echo    [%%i] !VARIANT_%%i!
        )
    )
    echo    [A] All of them
    echo.
    set /p "PICK=Which variant do you want to uninstall? "
    if /i "!PICK!"=="A" (
        for /l %%i in (1,1,%VARIANT_COUNT%) do set "SELECTED=!SELECTED! !VARIANT_%%i!"
    ) else (
        for /l %%i in (1,1,%VARIANT_COUNT%) do if "!PICK!"=="%%i" set "SELECTED=!VARIANT_%%i!"
        if "!SELECTED!"=="" (
            echo Invalid choice. Cancelled.
            pause
            exit /b 1
        )
    )
)

echo.
if not "%SELECTED%"=="" (
    echo This will remove, for variant^(s^)%SELECTED%:
    echo   - All post-processing shader DLLs from global plugins
    echo   - Their license files
    echo.
    echo Presets, settings and shader assets are shared by every build line, so
    echo they are only removed once no build line is left installed.
    echo.
)
if "%LEGACY_FOUND%"=="1" echo Leftovers from pre-variant releases of these same shaders will also go.
echo.
echo Third-party plugins and any variant you do not select are left untouched.
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

for %%v in (%SELECTED%) do call :remove_variant "%%v"

call :remove_legacy

call :remove_shared_data

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

:: --------------------------------------------------------------------------
:: :remove_variant <variant-name>
::
:: Only the variant's own plugin DLL tree, plus any pre-sharing data dirs still
:: qualified by this variant name. Shared data is handled by :remove_shared_data.
:: --------------------------------------------------------------------------
:remove_variant
set "V=%~1"
set "V_PLUGIN_DIR=%PLUGIN_ROOT%\%V%"
set "V_DATA_DIR=%DATA_ROOT%\%V%"

echo.
echo --- %V% ---

:: Shader DLLs and their license files. Match pattern "*Shader.dll".
:: Every plugin in this suite is built with that suffix, and the whole variant
:: directory belongs to this build, so the glob catches every past / future
:: shader rename or removal without tracking a list of historical names.
:: The NN_ prefix is deliberately NOT required: assign_shader_order.py excludes
:: some plugins (Bloom) from prefixing, and those would otherwise be left behind.
for /f "delims=" %%f in ('dir /b "%V_PLUGIN_DIR%\*Shader.dll" 2^>nul') do (
    del /f "%V_PLUGIN_DIR%\%%f" 2>&1
    if exist "%V_PLUGIN_DIR%\%%f" (
        echo   FAILED: %%f
        set /a ERRORS+=1
    ) else (
        echo   Removed: %%f
        set /a REMOVED+=1
    )
)
for /f "delims=" %%f in ('dir /b "%V_PLUGIN_DIR%\*Shader-LICENSE.txt" 2^>nul') do (
    del /f "%V_PLUGIN_DIR%\%%f" 2>&1
    if not exist "%V_PLUGIN_DIR%\%%f" set /a REMOVED+=1
)

:: The installer drops a copy of this script next to the DLLs; remove it too,
:: otherwise the variant directory can never be emptied.
if exist "%V_PLUGIN_DIR%\uninstall-plugins.bat" (
    del /f "%V_PLUGIN_DIR%\uninstall-plugins.bat" >nul 2>&1
    if not exist "%V_PLUGIN_DIR%\uninstall-plugins.bat" set /a REMOVED+=1
)

:: Global data left over from when data was still variant-scoped.
for %%d in (shipping_presets presets shader_assets shader_settings) do (
    if exist "%V_DATA_DIR%\%%d" (
        rmdir /s /q "%V_DATA_DIR%\%%d" >nul 2>&1
        if errorlevel 1 (
            echo   FAILED: could not remove %%d
            set /a ERRORS+=1
        ) else (
            echo   Removed: %%d
            set /a REMOVED+=1
        )
    )
)

:: Per-game data, likewise only present on installs predating shared data.
for /d %%g in ("%UEVR_DATA%\*") do (
    if exist "%%g\data\plugins\%V%" (
        rmdir /s /q "%%g\data\plugins\%V%" >nul 2>&1
        if not errorlevel 1 set /a REMOVED+=1
    )
)

:: Drop the now-empty variant dirs so no stale folder is left behind.
rmdir "%V_PLUGIN_DIR%" >nul 2>&1
rmdir "%V_DATA_DIR%" >nul 2>&1
goto :eof

:: --------------------------------------------------------------------------
:: :remove_legacy
::
:: Pre-variant leftovers. These carry no variant attribution, so they are shared
:: across every build and are removed once, regardless of which variant(s) were
:: selected. Only the exact names our backend writes are removed; the containing
:: plugins dirs are never removed, because other plugins live there too.
:: --------------------------------------------------------------------------
:remove_legacy
echo.
echo --- legacy (pre-variant) ---

for /f "delims=" %%f in ('dir /b "%PLUGIN_ROOT%\*Shader.dll" 2^>nul') do (
    del /f "%PLUGIN_ROOT%\%%f" 2>&1
    if exist "%PLUGIN_ROOT%\%%f" (
        echo   FAILED: %%f
        set /a ERRORS+=1
    ) else (
        echo   Removed: %%f
        set /a REMOVED+=1
    )
)
for /f "delims=" %%f in ('dir /b "%PLUGIN_ROOT%\*Shader-LICENSE.txt" 2^>nul') do (
    del /f "%PLUGIN_ROOT%\%%f" 2>&1
    if not exist "%PLUGIN_ROOT%\%%f" set /a REMOVED+=1
)

:: Pre-variant installs also dropped this script directly in plugins\.
if exist "%PLUGIN_ROOT%\uninstall-plugins.bat" (
    del /f "%PLUGIN_ROOT%\uninstall-plugins.bat" >nul 2>&1
    if not exist "%PLUGIN_ROOT%\uninstall-plugins.bat" set /a REMOVED+=1
)
goto :eof

:: --------------------------------------------------------------------------
:: :remove_shared_data
::
:: Presets, settings and shader assets are unqualified: every build line reads
:: and writes the same files. Removing them while another line is still
:: installed would destroy that line's configuration, so this runs only once no
:: build line remains -- i.e. no plugins\<variant>\ dir holds a *Shader.dll and
:: no legacy unqualified *Shader.dll is left in plugins\ either.
:: --------------------------------------------------------------------------
:remove_shared_data
set "STILL_INSTALLED=0"
for /d %%v in ("%PLUGIN_ROOT%\*") do (
    for /f "delims=" %%f in ('dir /b "%%v\*Shader.dll" 2^>nul') do set "STILL_INSTALLED=1"
)
for /f "delims=" %%f in ('dir /b "%PLUGIN_ROOT%\*Shader.dll" 2^>nul') do set "STILL_INSTALLED=1"

if "%STILL_INSTALLED%"=="1" (
    echo.
    echo --- shared data ---
    echo   Kept: another build line is still installed.
    goto :eof
)

echo.
echo --- shared data ---

for %%d in (shipping_presets presets shader_assets shader_settings) do (
    if exist "%DATA_ROOT%\%%d" (
        rmdir /s /q "%DATA_ROOT%\%%d" >nul 2>&1
        if not errorlevel 1 (
            echo   Removed: %%d
            set /a REMOVED+=1
        )
    )
)

for /d %%g in ("%UEVR_DATA%\*") do (
    for %%d in (shader_settings presets) do (
        if exist "%%g\data\plugins\%%d" (
            rmdir /s /q "%%g\data\plugins\%%d" >nul 2>&1
            if not errorlevel 1 set /a REMOVED+=1
        )
    )
    if exist "%%g\data\plugins\active_preset.txt" (
        del /f "%%g\data\plugins\active_preset.txt" >nul 2>&1
        if not exist "%%g\data\plugins\active_preset.txt" set /a REMOVED+=1
    )
    for %%f in ("%%g\data\plugins\*_settings.txt") do (
        del /f "%%f" >nul 2>&1
        if not exist "%%f" set /a REMOVED+=1
    )
)
goto :eof
