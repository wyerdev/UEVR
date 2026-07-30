// [fork-file] Ours — not in praydog/UEVR. Free to edit; no merge risk.
#pragma once

// [fork] variant-isolation: compile-time build-variant identity.
//
// Several independent builds of this fork can be installed side by side (the
// baseline shader build and the AFW build, for example). They ship different
// UEVRBackend.dll ABIs and different plugin DLLs, but they all read from
// %APPDATA%\UnrealVRMod. Without a variant qualifier the loader would happily
// pick up another build's plugin DLLs and another build's presets/settings.
//
// Every variant-scoped path is qualified by UEVR_VARIANT_ID, and every plugin
// exports its own variant ID so the loader can reject a foreign DLL even when
// it is sitting in the correct directory.
//
// The value is per-branch. Keep it in sync with the installer scripts and with
// deploy.sh.

#ifndef UEVR_VARIANT_ID
#define UEVR_VARIANT_ID "reshade"
#endif

#define UEVR_VARIANT_WIDEN_(x) L##x
#define UEVR_VARIANT_WIDEN(x) UEVR_VARIANT_WIDEN_(x)
#define UEVR_VARIANT_ID_W UEVR_VARIANT_WIDEN(UEVR_VARIANT_ID)

// Name of the identity-handshake export every plugin provides via Plugin.hpp.
#define UEVR_PLUGIN_VARIANT_EXPORT "uevr_plugin_variant_id"
