#!/usr/bin/env python3
"""Fail CI if repository metadata reintroduces dependency/release version pins."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ERRORS: list[str] = []


def fail(path: Path, message: str) -> None:
    ERRORS.append(f"{path.relative_to(ROOT)}: {message}")


def cmake_files() -> list[Path]:
    files = [ROOT / "CMakeLists.txt"]
    files.extend(ROOT.rglob("CMakeLists.txt"))
    files.extend(ROOT.rglob("*.cmake"))
    files.extend(ROOT.rglob("*.cmake.in"))
    return sorted({p for p in files if p.is_file() and "build" not in p.parts})


def check_cmake(path: Path) -> None:
    text = path.read_text(encoding="utf-8")

    for match in re.finditer(
        r"\b(find_package|find_dependency)\s*\((.*?)\)",
        text,
        flags=re.IGNORECASE | re.DOTALL,
    ):
        call, body = match.group(1), match.group(2)
        tokens = re.findall(r'[^\s"\']+', body)
        if len(tokens) >= 2 and re.fullmatch(r"\d+(?:\.\d+)*", tokens[1]):
            fail(path, f"{call} selects numeric version {tokens[1]}")
        if re.search(r"\bEXACT\b", body, flags=re.IGNORECASE):
            fail(path, f"{call} uses EXACT")

    for match in re.finditer(
        r"\bproject\s*\((.*?)\)",
        text,
        flags=re.IGNORECASE | re.DOTALL,
    ):
        if re.search(r"\bVERSION\b", match.group(1), flags=re.IGNORECASE):
            fail(path, "project() must not set a release VERSION")

    for match in re.finditer(
        r"\bset_target_properties\s*\((.*?)\)",
        text,
        flags=re.IGNORECASE | re.DOTALL,
    ):
        body = match.group(1)
        if re.search(r"\bSOVERSION\b", body, flags=re.IGNORECASE):
            fail(path, "target SOVERSION is forbidden")
        if re.search(r"\bVERSION\s+(?:\d|\$\{PROJECT_VERSION\})", body, flags=re.IGNORECASE):
            fail(path, "target VERSION is forbidden")

    if re.search(r"\bwrite_basic_package_version_file\s*\(", text, flags=re.IGNORECASE):
        fail(path, "generated CMake package-version files are forbidden")
    if re.search(r"ConfigVersion\.cmake", text, flags=re.IGNORECASE):
        fail(path, "CMake package-version file reference is forbidden")


def walk_json(value: object, path: Path, prefix: str = "") -> None:
    forbidden_keys = {
        "builtin-baseline",
        "version",
        "version-string",
        "version-semver",
        "version-date",
        "version>=",
        "overrides",
    }
    if isinstance(value, dict):
        for key, child in value.items():
            here = f"{prefix}.{key}" if prefix else key
            if key in forbidden_keys:
                fail(path, f"vcpkg manifest key {here!r} is forbidden")
            walk_json(child, path, here)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            walk_json(child, path, f"{prefix}[{index}]")


def check_vcpkg() -> None:
    root = ROOT / "cmake" / "vcpkg"
    for path in sorted(root.rglob("vcpkg.json")):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            fail(path, f"invalid JSON: {exc}")
            continue
        walk_json(data, path)


def check_workflows() -> None:
    root = ROOT / ".github" / "workflows"
    pattern = re.compile(r"\bSALTS_SDK_VERSION\s*[:=]\s*['\"]?\d", re.IGNORECASE)
    for path in sorted(list(root.glob("*.yml")) + list(root.glob("*.yaml"))):
        if pattern.search(path.read_text(encoding="utf-8")):
            fail(path, "workflow pins SALTS_SDK_VERSION")


def check_salts_restore() -> None:
    path = ROOT / "cmake" / "ci" / "restore-salts-sdk.ps1"
    text = path.read_text(encoding="utf-8")

    if re.search(
        r'PackageReference[^>]*Include="Salts\.Native"[^>]*Version="\s*[\[\(]?\d',
        text,
        flags=re.IGNORECASE | re.DOTALL,
    ):
        fail(path, "Salts.Native PackageReference uses a literal numeric version")

    if re.search(
        r"SALTS_SDK_VERSION.{0,200}?else\s*\{\s*['\"]\d",
        text,
        flags=re.IGNORECASE | re.DOTALL,
    ):
        fail(path, "SALTS_SDK_VERSION has a numeric default")


def main() -> int:
    for path in cmake_files():
        check_cmake(path)
    check_vcpkg()
    check_workflows()
    check_salts_restore()

    if ERRORS:
        print("Unversioned dependency policy violations:", file=sys.stderr)
        for error in ERRORS:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print("Unversioned dependency policy: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
