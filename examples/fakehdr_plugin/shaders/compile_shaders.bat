@echo off
rem Compile FakeHDR shaders to .inc bytecode headers

setlocal
set error=0

if %PROCESSOR_ARCHITECTURE%.==ARM64. (set FXCARCH=arm64) else (if %PROCESSOR_ARCHITECTURE%.==AMD64. (set FXCARCH=x64) else (set FXCARCH=x86))

set FXCOPTS=/nologo /WX /Ges /Zi /Zpc /Qstrip_reflect /Qstrip_debug

set PCFXC="%WindowsSdkVerBinPath%%FXCARCH%\fxc.exe"
if exist %PCFXC% goto continue
set PCFXC="%WindowsSdkBinPath%%WindowsSDKVersion%\%FXCARCH%\fxc.exe"
if exist %PCFXC% goto continue
set PCFXC="%WindowsSdkDir%bin\%WindowsSDKVersion%\%FXCARCH%\fxc.exe"
if exist %PCFXC% goto continue

set PCFXC=fxc.exe

:continue

@if not exist "Compiled" mkdir "Compiled"

echo Compiling FakeHDR Vertex Shader...
%PCFXC% "FakeHDR_VS.hlsl" %FXCOPTS% /Tvs_5_0 /Emain "/FhCompiled\FakeHDR_VS.inc" "/FdCompiled\FakeHDR_VS.pdb" /VnFakeHDR_VS_bytecode || set error=1

echo Compiling FakeHDR Pixel Shader...
%PCFXC% "FakeHDR_PS.hlsl" %FXCOPTS% /Tps_5_0 /Emain "/FhCompiled\FakeHDR_PS.inc" "/FdCompiled\FakeHDR_PS.pdb" /VnFakeHDR_PS_bytecode || set error=1

if %error% == 0 (
    echo FakeHDR shaders compiled OK
) else (
    echo Shader compilation FAILED!
    exit /b 1
)

endlocal
exit /b 0
