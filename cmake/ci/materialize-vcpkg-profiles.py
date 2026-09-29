#!/usr/bin/env python3
import json
import pathlib
import sys

if len(sys.argv) != 3:
    raise SystemExit(
        "usage: materialize-vcpkg-profiles.py <profiles-root> <shared-cache-root>"
    )

profiles_root = pathlib.Path(sys.argv[1])
shared_cache_root = pathlib.Path(sys.argv[2])
shared_manifest_path = shared_cache_root / "vcpkg.json"

with shared_manifest_path.open("r", encoding="utf-8") as f:
    shared_manifest = json.load(f)

baseline = shared_manifest.get("builtin-baseline")
if not isinstance(baseline, str) or not baseline:
    raise SystemExit(
        f"shared cache manifest has no builtin-baseline: {shared_manifest_path}"
    )

profiles = sorted(profiles_root.glob("*/vcpkg.json"))
if not profiles:
    raise SystemExit(f"no vcpkg profiles found under {profiles_root}")

for manifest_path in profiles:
    with manifest_path.open("r", encoding="utf-8") as f:
        manifest = json.load(f)

    manifest["builtin-baseline"] = baseline

    with manifest_path.open("w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print(f"Bound {manifest_path} to shared cache baseline")
