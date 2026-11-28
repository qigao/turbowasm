"""Restore the released, architecture-matched SDK set using shared curl transport."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile

ROOT = Path.cwd()
LOCK = json.loads((ROOT / "cmake/ci/sdk-lock.json").read_text(encoding="utf-8"))
profile = sys.argv[1]
triplet = sys.argv[2]
selected = LOCK["profiles"][profile]
if triplet != selected["triplet"]:
    raise ValueError("Profile/triplet mismatch")
missing = [p["repo"] for p in LOCK["packages"] if p["repo"] not in selected["assets"]]
if missing:
    raise RuntimeError(f"{profile}: no published architecture-matched SDKs for {', '.join(missing)}; {triplet} cache is also not published. Publish matching upstream inputs and update sdk-lock.json.")
infra = ROOT / "build/ci-infrastructure/vcpkg-cache"
reader = infra / "scripts/restore-native-dependency.py"
spec = importlib.util.spec_from_file_location("transport", infra / "scripts/setup-compiler-toolchain.py")
transport = importlib.util.module_from_spec(spec)
spec.loader.exec_module(transport)
inputs = ROOT / "build/ci-inputs"
inputs.mkdir(parents=True, exist_ok=True)
deps = ROOT / "build/dependencies"
deps.mkdir(parents=True, exist_ok=True)
env = {"VCPKG_CACHE_VERSION": LOCK["cache"], "VCPKG_TARGET_TRIPLET": triplet}
version_keys = {"salts-base": "SALTS_BASE_VERSION", "cmeta": "CMETA_VERSION",
                "salts-utils": "SALTS_UTILS_VERSION", "salts-cnet": "SALTS_CORE_VERSION",
                "chttp": "CHTTP_VERSION"}

def acquire(asset):
    path = inputs / asset["name"]
    transport.verified_download(asset["url"], path, asset["sha256"], asset["size"])
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if path.stat().st_size != asset["size"] or digest != asset["sha256"]:
        raise ValueError(f"Invalid release asset {asset['name']}")
    return path

def run(*args):
    subprocess.run([sys.executable, str(reader), *map(str, args)], check=True,
                   env={**os.environ, **env})

def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

# The lock binds each release to a successful producer at its exact source commit.
# At lock creation, the release API's canonical URLs, sizes, digests and
# producer run identity were verified together. Upstream manifests have
# different schemas; retain their exact authenticated bytes as provenance
# rather than assuming that all producers serialize the same field names.
for package in LOCK["packages"]:
    name = package["repo"]
    asset = selected["assets"][name]
    acquire(package["manifest"])
    producer = package["producer"]
    root = deps / (name + "-" + package["version"] + "-" + profile)
    root.mkdir()
    with tarfile.open(acquire(asset)) as archive:
        archive.extractall(root, filter="data")
    write_json(root / "sdk-restore-receipt.json", {
        "version": package["version"], "producer": producer, "profile": asset["profile"],
        "sha256": asset["sha256"], "source": package["source"],
        "release_id": package["release_id"], "manifest_sha256": package["manifest"]["sha256"],
    })
    env[package["root"]] = root.as_posix()
    env[version_keys[name]] = package["version"]

# Resolve and restore the exact preinstalled vcpkg release, never build ports.
descriptor = inputs / "cache-descriptor.json"
write_json(descriptor, {"schema": 1, "source": {"kind": "github-release",
    "repository": "qigao/vcpkg-cache", "version_env": "VCPKG_CACHE_VERSION",
    "asset": f"installed-{triplet}.tar.xz"}})
cache_lock = inputs / "cache.lock.json"
run("resolve-release", descriptor, "--output-lock", cache_lock)
cache_root = deps / ("vcpkg-" + triplet)
run("locked", cache_lock, "--destination", cache_root, "--payload", "vcpkg_installed")
env["VCPKG_INSTALLED_DIR"] = json.loads((cache_root / "receipt.json").read_text(encoding="utf-8"))["root"]
env["SALTS_CNET_ROOT"] = env["SALTSCORE_ROOT"]
env["HTTP_SERVICES_ROOT"] = env["CHTTP_ROOT"]

# Native Windows toolchains come from the cache's published compiler contract.
if profile.startswith("windows-llvm-mingw-"):
    arch = profile.rsplit("-", 1)[-1]
    compiler = json.loads((infra / "compiler-contracts/release-obfuscation.json").read_text(encoding="utf-8"))["sources"]["llvm-mingw"]
    asset = next(a for a in compiler["assets"] if a["host"] == "windows-" + arch)
    compiler_lock = inputs / "compiler.lock.json"
    write_json(compiler_lock, {"schema": 1, "source": {"kind": "url", "url":
        f"https://github.com/{compiler['repository']}/releases/download/{compiler['release_tag']}/{asset['name']}"},
        "sha256": asset["sha256"]})
    compiler_root = deps / "llvm-mingw"
    target = {"x64": "x86_64-w64-mingw32", "arm64": "aarch64-w64-mingw32"}[arch]
    run("locked", compiler_lock, "--destination", compiler_root,
        "--payload", Path(asset["name"]).stem, "--executable", f"bin/{target}-clang.exe")
    env["LLVM_MINGW_ROOT"] = json.loads((compiler_root / "receipt.json").read_text(encoding="utf-8"))["root"]
    env["LLVM_MINGW_TARGET"] = target

env["LLVM_NATIVE_TARGET_TRIPLET"] = triplet
stage = (ROOT / "stage" / profile).as_posix()
for key in ("LLVM_NATIVE_INSTALL_PREFIX", "LLVM_MINGW_X64_INSTALL_PREFIX",
            "LLVM_MINGW_ARM64_INSTALL_PREFIX", "ANDROID_X64_INSTALL_PREFIX", "ANDROID_ARM64_INSTALL_PREFIX"):
    env[key] = stage

with Path(os.environ["GITHUB_ENV"]).open("a", encoding="utf-8") as output:
    for key, value in env.items():
        output.write(f"{key}={str(value).replace(chr(92), '/')}\n")
# Native tests need the DLL/shared-library search roots of the complete SDK set.
roots = [env[p["root"]] for p in LOCK["packages"]]
if profile.startswith("windows-"):
    with Path(os.environ["GITHUB_PATH"]).open("a", encoding="utf-8") as output:
        for root in roots:
            output.write(f"{root}/bin\n")
        output.write(f"{env['LLVM_MINGW_ROOT']}/bin\n")
        output.write(f"{env['LLVM_MINGW_ROOT']}/{env['LLVM_MINGW_TARGET']}/bin\n")
elif not profile.startswith("android-"):
    variable = "DYLD_LIBRARY_PATH" if profile.startswith("macos-") else "LD_LIBRARY_PATH"
    with Path(os.environ["GITHUB_ENV"]).open("a", encoding="utf-8") as output:
        output.write(f"{variable}={os.pathsep.join(root + '/lib' for root in roots)}\n")
