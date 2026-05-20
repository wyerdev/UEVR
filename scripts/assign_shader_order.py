#!/usr/bin/env python3
"""Assign sequential NN_ prefixes to shader plugin DLLs and LICENSE files.

Reads render_order() from each plugin's .cpp source, sorts plugins by that
value, and renames DLLs (and optionally LICENSE files) in a target directory
with sequential two-digit prefixes.

Usage:
    python scripts/assign_shader_order.py <staging_dir> [--exclude Bloom]

The staging directory should contain bare-named DLLs (e.g. AdaptiveTonemapperShader.dll)
as produced by the build. The script renames them in-place to NN_Name.dll.

For LICENSE files, pass --license-src <dir> to copy LICENSE files from examples/
into the staging dir with the correct prefix.
"""

import argparse
import re
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
EXAMPLES_DIR = REPO_ROOT / "examples"

# Regex to extract render_order() value from a plugin .cpp
RENDER_ORDER_RE = re.compile(
    r"int\s+render_order\s*\(\s*\)\s*const\s+override\s*\{\s*return\s+(\d+)\s*;\s*\}"
)

# Regex to extract the shader base name from a DLL filename (strip any existing NN_ prefix)
PREFIX_RE = re.compile(r"^(\d+_(?:\d+_|[A-Z]_)?)?(.+)$")


def find_plugin_orders() -> list[tuple[int, str, Path]]:
    """Scan examples/ for render_order() values.

    Returns sorted list of (order, shader_base_name, plugin_dir).
    The shader base name is derived from the .cpp filename: FooPlugin.cpp -> FooShader.
    """
    results = []
    for plugin_dir in EXAMPLES_DIR.iterdir():
        if not plugin_dir.is_dir():
            continue
        # Find the main .cpp (named *Plugin.cpp)
        cpps = list(plugin_dir.glob("*Plugin.cpp"))
        if not cpps:
            continue
        cpp_file = cpps[0]
        text = cpp_file.read_text(encoding="utf-8", errors="replace")
        m = RENDER_ORDER_RE.search(text)
        if not m:
            continue
        order = int(m.group(1))
        # Derive shader name: FooPlugin.cpp -> FooShader
        stem = cpp_file.stem  # e.g. "AdaptiveTonemapperPlugin"
        if stem.endswith("Plugin"):
            shader_name = stem[: -len("Plugin")] + "Shader"
        else:
            shader_name = stem + "Shader"
        results.append((order, shader_name, plugin_dir))

    results.sort(key=lambda x: x[0])
    return results


def build_prefix_map(
    orders: list[tuple[int, str, Path]], exclude: list[str]
) -> dict[str, str]:
    """Build mapping: bare shader name -> prefixed shader name (e.g. "FakeHDRShader" -> "08_FakeHDRShader").

    Plugins whose shader name matches any exclude pattern are skipped.
    """
    mapping = {}
    idx = 0
    for _order, shader_name, _plugin_dir in orders:
        if any(ex.lower() in shader_name.lower() for ex in exclude):
            continue
        prefix = f"{idx:02d}"
        mapping[shader_name] = f"{prefix}_{shader_name}"
        idx += 1
    return mapping


def rename_dlls(staging_dir: Path, mapping: dict[str, str]) -> int:
    """Rename DLLs in staging_dir using the prefix mapping. Returns count renamed."""
    count = 0
    for dll in list(staging_dir.glob("*.dll")):
        # Strip any existing prefix to get bare name
        m = PREFIX_RE.match(dll.stem)
        if not m:
            continue
        bare = m.group(2)
        if bare in mapping:
            new_name = mapping[bare] + ".dll"
            target = staging_dir / new_name
            if dll.name != new_name:
                # Remove any conflicting file first (stale old-prefix builds)
                if target.exists():
                    target.unlink()
                dll.rename(target)
            count += 1
        else:
            # DLL doesn't match any known shader — remove (stale artifact)
            dll.unlink()
    return count


def copy_licenses(staging_dir: Path, mapping: dict[str, str]) -> int:
    """Copy LICENSE files from examples/ into staging_dir with correct prefix."""
    count = 0
    for plugin_dir in EXAMPLES_DIR.iterdir():
        if not plugin_dir.is_dir():
            continue
        for lic in plugin_dir.glob("*-LICENSE.txt"):
            # Extract bare shader name from license filename
            lic_m = PREFIX_RE.match(lic.stem.replace("-LICENSE", ""))
            if not lic_m:
                continue
            bare = lic_m.group(2)
            if bare not in mapping:
                continue
            new_name = f"{mapping[bare]}-LICENSE.txt"
            shutil.copy2(lic, staging_dir / new_name)
            count += 1
    return count


def print_mapping(mapping: dict[str, str]) -> None:
    """Print the mapping to stdout for integration with other tools."""
    for bare, prefixed in mapping.items():
        print(f"{bare} -> {prefixed}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "staging_dir",
        nargs="?",
        type=Path,
        help="Directory containing built shader DLLs to rename",
    )
    parser.add_argument(
        "--exclude",
        nargs="*",
        default=["Bloom"],
        help="Shader name substrings to exclude (default: Bloom)",
    )
    parser.add_argument(
        "--license-src",
        action="store_true",
        help="Also copy and rename LICENSE files from examples/ into staging_dir",
    )
    parser.add_argument(
        "--print-map",
        action="store_true",
        help="Print the bare->prefixed mapping and exit",
    )
    parser.add_argument(
        "--print-dlls",
        action="store_true",
        help="Print the final DLL names (one per line) and exit",
    )
    args = parser.parse_args()

    orders = find_plugin_orders()
    if not orders:
        print("ERROR: No plugins found in examples/", file=sys.stderr)
        return 1

    mapping = build_prefix_map(orders, args.exclude)

    if args.print_map:
        print_mapping(mapping)
        return 0

    if args.print_dlls:
        for prefixed in mapping.values():
            print(f"{prefixed}.dll")
        return 0

    if not args.staging_dir:
        parser.error("staging_dir is required unless --print-map or --print-dlls is used")

    staging = args.staging_dir.resolve()
    if not staging.is_dir():
        print(f"ERROR: {staging} is not a directory", file=sys.stderr)
        return 1

    renamed = rename_dlls(staging, mapping)
    print(f"Renamed {renamed} DLLs")

    if args.license_src:
        copied = copy_licenses(staging, mapping)
        print(f"Copied {copied} LICENSE files")

    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
