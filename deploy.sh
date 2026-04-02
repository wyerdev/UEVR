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

# Deploy shader DLLs and their license files
PLUGIN_SRC="$SCRIPT_DIR/build/Release"
PLUGIN_DST="$UEVR_DATA/UEVR/plugins"
mkdir -p "$PLUGIN_DST"
for dll in "$PLUGIN_SRC"/*Shader.dll; do
  if [[ -f "$dll" ]]; then
    cp -f "$dll" "$PLUGIN_DST/"
    echo "  Copied $(basename "$dll")"
    ((COPIED++))
  fi
done

# Deploy per-plugin license files
for lic in "$SCRIPT_DIR"/examples/*/*-LICENSE.txt; do
  if [[ -f "$lic" ]]; then
    cp -f "$lic" "$PLUGIN_DST/"
    echo "  Copied $(basename "$lic")"
    ((COPIED++))
  fi
done

# Deploy shipping presets (always overwrite — these are built-in, not user presets)
PRESET_SRC="$SCRIPT_DIR/presets"
PRESET_DST="$UEVR_DATA/UEVR/data/plugins/shipping_presets"
if [[ -d "$PRESET_SRC" ]]; then
  mkdir -p "$PRESET_DST"
  for preset_dir in "$PRESET_SRC"/*/; do
    preset_name="$(basename "$preset_dir")"
    mkdir -p "$PRESET_DST/$preset_name"
    cp -f "$preset_dir"* "$PRESET_DST/$preset_name/"
    echo "  Deployed preset: $preset_name"
    ((COPIED++))
  done
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
