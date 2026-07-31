#!/bin/bash
# Deploy built UEVR DLLs to this build line's runtime directory.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UEVR_DATA="$APPDATA/UnrealVRMod"

# [fork] afw-joeyhodge: this branch builds out-of-source and installs to its
# own runtime dir, kept separate from the reshade and afw lines.
BUILD_DIR="A:/UEVR-build/afw-joeyhodge"
SRC="$BUILD_DIR/bin"
DST="A:/UEVR-build/afw-joeyhodge-run"
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

# [fork] do NOT deploy the built PDAFWPlugin.dll. The `pdafwmod` target in
# cmake.toml builds a dummy stub (dependencies/pd-afwmod/dummy) whose InitDevice
# returns nullptr; it exists only to produce an import library for the backend's
# /DELAYLOAD:PDAFWPlugin.dll. The real plugin is a prebuilt proprietary binary
# from the AFW release zip and must be placed in $DST by hand.
if [[ ! -f "$DST/PDAFWPlugin.dll" ]]; then
  echo "  WARNING: $DST/PDAFWPlugin.dll missing - frame warp will be disabled."
  echo "           Extract it from the AFW release zip; do not use the build output."
fi


# Deploy shader DLLs and their license files with sequential NN_ prefixes
# [fork] variant-isolation: must match UEVR_VARIANT_ID in include/uevr/Variant.hpp.
# Only the plugin DLL dir is variant-scoped; presets/settings/assets are shared
# by every build line.
VARIANT="reshade-afw-joeyhodge"
PLUGIN_DST="$UEVR_DATA/UEVR/plugins/$VARIANT"
mkdir -p "$PLUGIN_DST"

# Create a staging directory
STAGE_TMP=$(mktemp -d 2>/dev/null || (mkdir -p "$BUILD_DIR/deploy_stage" && echo "$BUILD_DIR/deploy_stage"))

# Copy bare-named DLLs to staging
cp -f "$BUILD_DIR/Release"/*Shader.dll "$STAGE_TMP/"

# Assign sequential NN_ prefixes from render_order() and copy LICENSE files
python "$SCRIPT_DIR/scripts/assign_shader_order.py" "$STAGE_TMP" --exclude Bloom --copy-licenses

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
