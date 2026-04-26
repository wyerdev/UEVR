# Stale .obj guard for build.bat.
#
# MSBuild's incremental dependency tracking does not always recompile a .cpp
# when a header it includes has changed (particularly when the change adds a
# field to a struct that crosses a DLL boundary). The result is silent ABI
# mismatch at runtime with no build-time error. See COMPILING.md
# "Build-system gotchas" for the full postmortem.
#
# Strategy: scan every first-party header in the repo (src/, include/,
# examples/, lua-api/, vr-plugin-nullifier/, side-projects/) and find the
# newest mtime. Then for every MSBuild intermediate dir under build/ that
# contains .obj files compiled from first-party sources, if any .obj is older
# than the newest header, delete every .obj in that dir. MSBuild will
# recompile on the next pass.
#
# We deliberately over-delete (entire intermediate dir, not just the TUs that
# include the changed header) — MSBuild dependency tracking is the thing we
# don't trust, so we don't try to be clever here. The full UEVR build is only
# tens of TUs per target, so the cost of a forced recompile is negligible
# compared to the cost of shipping an ABI-corrupted DLL.
#
# Skipped:
# - build/_deps/        third-party (CMake FetchContent), tracked correctly
# - build/<anything>/CMakeFiles/   CMake's own intermediates, not MSVC obj dirs
# - dependencies/       submodules / vendored libs

$ErrorActionPreference = 'Stop'

# Resolve repo root (script lives in <repo>/scripts/)
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

# 1. Newest first-party header mtime.
$srcDirCandidates = @('src', 'include', 'examples', 'lua-api',
                      'vr-plugin-nullifier', 'side-projects')
$srcDirs = $srcDirCandidates | Where-Object { Test-Path $_ }
if ($srcDirs.Count -eq 0) {
    Write-Host "  No first-party source dirs found; skipping stale-obj guard."
    exit 0
}

$hdrs = Get-ChildItem -Path $srcDirs -Recurse -Include *.hpp, *.h, *.hxx, *.inl `
                      -ErrorAction SilentlyContinue
if (-not $hdrs -or $hdrs.Count -eq 0) {
    Write-Host "  No headers found under first-party source dirs; skipping."
    exit 0
}

$newestHdr     = ($hdrs | Sort-Object LastWriteTime -Descending | Select-Object -First 1)
$newestHdrTime = $newestHdr.LastWriteTime
Write-Host ("  Newest header: {0} ({1:HH:mm:ss})" -f $newestHdr.Name, $newestHdrTime)

# 2. For each MSBuild intermediate dir under build/ matching <target>.dir/<config>,
#    if any .obj is older than the newest header, delete every .obj in that dir.
if (-not (Test-Path 'build')) {
    Write-Host "  No build/ dir; nothing to scan."
    exit 0
}

$skipPatterns = @('\_deps\', '\CMakeFiles\', '\dependencies\', '\_cmkr')

# Third-party target names whose .dir/* MSBuild intermediate dirs we leave
# alone. None of these compile our headers, so invalidating them only wastes
# build time. Keep this list in sync with cmake.toml when new third-party
# targets are added.
$thirdPartyTargets = @(
    'glm', 'lua', 'luavrlib', 'LuaVR', 'imgui', 'spdlog', 'openvr',
    'openxr_loader', 'safetyhook', 'sdkgenny', 'kananlib', 'asmjit',
    'sdk-test', 'cmkr', 'uesdk'
)

$objDirs = Get-ChildItem -Path 'build' -Recurse -Directory -ErrorAction SilentlyContinue |
           Where-Object {
               $_.Name -in @('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel') -and
               $_.Parent.Name -like '*.dir'
           } |
           Where-Object {
               $full = $_.FullName
               $skip = $false
               foreach ($pat in $skipPatterns) {
                   if ($full -like "*$pat*") { $skip = $true; break }
               }
               if (-not $skip) {
                   # $_.Parent.Name is like 'glm.dir' — strip '.dir' and check.
                   $targetName = $_.Parent.Name -replace '\.dir$', ''
                   if ($thirdPartyTargets -contains $targetName) { $skip = $true }
               }
               -not $skip
           }

$totalRemoved = 0
$dirsCleaned  = 0

foreach ($d in $objDirs) {
    $objs = Get-ChildItem -Path $d.FullName -Filter *.obj -ErrorAction SilentlyContinue
    if (-not $objs -or $objs.Count -eq 0) { continue }

    $oldestObj = ($objs | Sort-Object LastWriteTime | Select-Object -First 1).LastWriteTime
    if ($newestHdrTime -gt $oldestObj) {
        $relDir = $d.FullName.Substring($repoRoot.Length).TrimStart('\', '/')
        Write-Host ("  STALE in {0} (oldest obj {1:HH:mm:ss} < newest hdr {2:HH:mm:ss})" -f `
                    $relDir, $oldestObj, $newestHdrTime)
        foreach ($obj in $objs) {
            Remove-Item -Force $obj.FullName
            $totalRemoved++
        }
        $dirsCleaned++
    }
}

if ($totalRemoved -gt 0) {
    Write-Host ("  Removed {0} stale .obj file(s) across {1} dir(s); MSBuild will recompile." -f `
                $totalRemoved, $dirsCleaned)
} else {
    Write-Host "  No stale objects detected."
}

exit 0
