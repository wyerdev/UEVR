#!/bin/bash
# Deploy built UEVR DLLs to the active UEVR installation
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UEVR_DATA="$APPDATA/UnrealVRMod"

# [fork] this branch builds out-of-source and installs to its own runtime dir.
BUILD_DIR="${UEVR_BUILD_DIR:-A:/UEVR-build/reshade}"
SRC="$BUILD_DIR/bin"
DST="${UEVR_DEPLOY_DIR:-A:/UEVR-build/reshade-run}"
mkdir -p "$DST"

COPIED=0
for pair in \
  "uevr/UEVRBackend.dll:UEVRBackend.dll" \
  "uevr/UEVRBackend.pdb:UEVRBackend.pdb" \
  "uevr/openvr_api.dll:openvr_api.dll" \
  "vr-plugin-nullifier/UEVRPluginNullifier.dll:UEVRPluginNullifier.dll" \
  "vr-plugin-nullifier/UEVRPluginNullifier.pdb:UEVRPluginNullifier.pdb" \
  "LuaVR/LuaVR.dll:LuaVR.dll" \
  "LuaVR/LuaVR.pdb:LuaVR.pdb" \
; do
  src_file="${pair%%:*}"
  dst_file="${pair##*:}"
  if [[ -f "$SRC/$src_file" ]]; then
    cp "$SRC/$src_file" "$DST/$dst_file"
    echo "  Copied $dst_file"
    ((COPIED++))
  fi
done

# Deploy shader DLLs and their license files with sequential NN_ prefixes.
# [fork] all build lines load the same shared shader directory.
PLUGIN_ROOT="$UEVR_DATA/UEVR/plugins"
PLUGIN_DST="$PLUGIN_ROOT/shaders"
mkdir -p "$PLUGIN_DST"

# Create a staging directory
STAGE_TMP=$(mktemp -d 2>/dev/null || (mkdir -p "$BUILD_DIR/deploy_stage" && echo "$BUILD_DIR/deploy_stage"))

# Copy bare-named DLLs to staging
shopt -s nullglob
shader_sources=("$BUILD_DIR/Release"/*Shader.dll)
if [[ ${#shader_sources[@]} -eq 0 ]]; then
  echo "ERROR: No shader DLLs found in $BUILD_DIR/Release"
  rm -rf "$STAGE_TMP"
  exit 1
fi
cp -f "${shader_sources[@]}" "$STAGE_TMP/"

# Assign sequential NN_ prefixes from render_order() and copy LICENSE files
python "$SCRIPT_DIR/scripts/assign_shader_order.py" "$STAGE_TMP" --exclude Bloom --copy-licenses

# Clean up shared files and legacy shader files from the old global locations.
rm -f "$PLUGIN_ROOT"/*Shader.dll "$PLUGIN_ROOT"/*Shader-LICENSE.txt
for legacy_dir in "$PLUGIN_ROOT"/*; do
  [[ -d "$legacy_dir" ]] || continue
  [[ "$legacy_dir" == "$PLUGIN_DST" ]] && continue
  rm -f "$legacy_dir"/*Shader.dll "$legacy_dir"/*Shader-LICENSE.txt
done
rm -f "$PLUGIN_DST"/*Shader.dll "$PLUGIN_DST"/*Shader-LICENSE.txt

# Copy all files from staging to target
for file in "$STAGE_TMP"/*; do
  if [[ -f "$file" ]]; then
    cp -f "$file" "$PLUGIN_DST/"
    echo "  Copied $(basename "$file")"
    ((COPIED++))
  fi
done

# Clean up staging directory
rm -rf "$STAGE_TMP"
shopt -u nullglob

# Deploy shipping presets (always overwrite — these are built-in, not user presets)
PRESET_SRC="$SCRIPT_DIR/presets"
PRESET_DST="$UEVR_DATA/UEVR/data/plugins/shipping_presets"
if [[ -d "$PRESET_SRC" ]]; then
  mkdir -p "$PRESET_DST"
  # Copy flat files. The release zip and runtime only expects flat .uevrpreset files in shipping_presets.
  cp -f "$PRESET_SRC"/*.uevrpreset "$PRESET_DST/"
  echo "  Deployed presets from $PRESET_SRC"
  ((COPIED++))
fi

# Deploy shipped shader assets (LUTs, textures). The release zip ships these
# flat in `shader_assets/`; the dev tree splits them per plugin under
# `examples/<plugin>/assets/`, so flatten them here. Mirrors the per-plugin
# branch of install-plugins.bat.
ASSET_DST="$UEVR_DATA/UEVR/data/plugins/shader_assets"
shopt -s nullglob
asset_dirs=("$SCRIPT_DIR/examples"/*/assets)
if [[ ${#asset_dirs[@]} -gt 0 ]]; then
  mkdir -p "$ASSET_DST"
  for dir in "${asset_dirs[@]}"; do
    for file in "$dir"/*; do
      [[ -f "$file" ]] || continue
      cp -f "$file" "$ASSET_DST/"
      ((COPIED++))
    done
  done
  echo "  Deployed shader assets to $ASSET_DST"
fi
shopt -u nullglob

LOG="$UEVR_DATA/CreaturesOfAva-Win64-Shipping/log.txt"
if [[ -f "$LOG" ]]; then
  rm "$LOG"
  echo "  Deleted CreaturesOfAva log.txt"
fi

LOG2="$UEVR_DATA/Returnal-Win64-Shipping/log.txt"
if [[ -f "$LOG2" ]]; then
  rm "$LOG2"
  echo "  Deleted Returnal log.txt"
fi

echo ""
echo "Deployed $COPIED files to: $DST"
ls -la "$DST"/*.dll
