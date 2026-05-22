#!/bin/bash
# Deploy built UEVR DLLs to the active UEVR installation
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UEVR_DATA="$APPDATA/UnrealVRMod"

SRC="$SCRIPT_DIR/build/bin"
DST="A:/UEVR/uevr 2026-01-13 (1127) - Mine"

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

# Deploy shader DLLs and their license files with sequential NN_ prefixes
PLUGIN_DST="$UEVR_DATA/UEVR/plugins"
mkdir -p "$PLUGIN_DST"

# Create a staging directory
STAGE_TMP=$(mktemp -d 2>/dev/null || (mkdir -p "$SCRIPT_DIR/build/deploy_stage" && echo "$SCRIPT_DIR/build/deploy_stage"))

# Copy bare-named DLLs to staging
cp -f "$SCRIPT_DIR/build/Release"/*Shader.dll "$STAGE_TMP/"

# Assign sequential NN_ prefixes from render_order() and copy LICENSE files
python "$SCRIPT_DIR/scripts/assign_shader_order.py" "$STAGE_TMP" --exclude Bloom --license-src

# Clean up previously-installed shader DLLs and their license files (both prefixed and unprefixed)
rm -f "$PLUGIN_DST"/*Shader.dll
rm -f "$PLUGIN_DST"/*Shader-LICENSE.txt

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
