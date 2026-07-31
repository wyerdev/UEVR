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

# [fork] afw: do NOT deploy the built PDAFWPlugin.dll. The `pdafwmod` target in
# cmake.toml builds a dummy stub (dependencies/pd-afwmod/dummy) whose InitDevice
# returns nullptr; it exists only to produce an import library for the backend's
# /DELAYLOAD:PDAFWPlugin.dll. The real plugin is a prebuilt proprietary binary
# from the AFW release zip and must be placed in $DST by hand.
if [[ ! -f "$DST/PDAFWPlugin.dll" ]]; then
  echo "  WARNING: $DST/PDAFWPlugin.dll missing - frame warp will be disabled."
  echo "           Extract it from the AFW release zip; do not use the build output."
fi

# ---------------------------------------------------------------------------
# [2026-07-31] Shader-plugin, preset and shader-asset deployment is absent on
# purpose. None of it exists on this branch yet: no *Shader.dll targets, no
# presets/, no examples/*/assets, no scripts/assign_shader_order.py. Port those
# sections over from deploy.sh on wyerdev/afw in the same change that ports the
# shader plugins themselves, and set VARIANT to reshade-afw-joeyhodge then --
# it must match UEVR_VARIANT_ID in include/uevr/Variant.hpp.
# ---------------------------------------------------------------------------

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
