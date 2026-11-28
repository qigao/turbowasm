"""CI-only SDK acquisition using the shared curl transport and exact releases."""
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile

PINS = ("VCPKG_CACHE_VERSION", "SALTS_BASE_VERSION", "CMETA_VERSION",
        "SALTS_CORE_VERSION", "SALTS_UTILS_VERSION", "CHTTP_VERSION")
ROOT = Path.cwd()
BUILD = ROOT / "build"
CONTRACT = BUILD / "vcpkg-cache-contract" / "scripts"

def emit(name, value, file="GITHUB_ENV"):
    with open(os.environ[file], "a", encoding="utf-8") as stream:
        stream.write(f"{name}={value}\n")

def pins():
    values = dict(line.split("=", 1) for line in
                  (ROOT / ".env.example").read_text(encoding="utf-8").splitlines()
                  if "=" in line and not line.lstrip().startswith("#"))
    for key in PINS:
        value = values[key].strip().strip('"').strip("'")
        if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[A-Za-z0-9.]+)?", value):
            raise RuntimeError(f"Invalid exact SDK pin: {key}={value}")
        emit(key, value)
    emit("cache_version", values["VCPKG_CACHE_VERSION"].strip().strip('"').strip("'"), "GITHUB_OUTPUT")

def api(endpoint):
    return json.loads(subprocess.check_output(["gh", "api", endpoint], text=True))

def asset(release, name):
    matches = [item for item in release["assets"] if item["name"] == name]
    if len(matches) != 1:
        raise RuntimeError(f"{release['html_url']} does not publish required asset {name}")
    item = matches[0]
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", item.get("digest") or ""):
        raise RuntimeError(f"Missing SHA-256 release digest for {name}")
    return item

def download(helper, item, dest):
    helper.verified_download(item["browser_download_url"], dest,
                             item["digest"][7:], item["size"])
    if dest.stat().st_size != item["size"]:
        raise RuntimeError(f"Size mismatch: {dest}")

def restore():
    profile, triplet, arch = (os.environ[k] for k in ("SDK_PROFILE", "SDK_TRIPLET", "SDK_ARCH"))
    stage = BUILD / "ci-downloads"
    stage.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("transport", CONTRACT / "setup-compiler-toolchain.py")
    helper = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = helper
    spec.loader.exec_module(helper)
    descriptor = stage / "cache.json"
    lock = stage / "cache.lock.json"
    descriptor.write_text(json.dumps({"schema": 1, "source": {
        "kind": "github-release", "repository": "qigao/vcpkg-cache",
        "version_env": "VCPKG_CACHE_VERSION", "asset": f"installed-{triplet}.tar.xz"
    }}), encoding="utf-8")
    reader = CONTRACT / "restore-native-dependency.py"
    subprocess.run([sys.executable, str(reader), "resolve-release", str(descriptor),
                    "--output-lock", str(lock)], check=True)
    cache = BUILD / "ci-vcpkg"
    subprocess.run([sys.executable, str(reader), "locked", str(lock), "--destination",
                    str(cache), "--payload", "vcpkg_installed"], check=True)
    installed = Path(json.loads((cache / "receipt.json").read_text())["root"]).resolve()
    emit("VCPKG_INSTALLED_DIR", installed.as_posix())
    emit("VCPKG_TARGET_TRIPLET", triplet)
    emit("LLVM_NATIVE_TARGET_TRIPLET", triplet)
    emit("LLVM_MINGW_TARGET", "aarch64-w64-mingw32" if arch == "arm64" else "x86_64-w64-mingw32")
    emit("LLVM_MINGW_ARM64_VCPKG_INSTALLED_DIR", installed.as_posix())
    for key in ("LLVM_NATIVE", "LLVM_MINGW_X64", "LLVM_MINGW_ARM64", "ANDROID_X64", "ANDROID_ARM64"):
        emit(f"{key}_INSTALL_PREFIX", (BUILD / "ci-install").as_posix())
    cnet_profile = (profile.replace("linux-clang-", "linux-")
                    .replace("macos-clang-", "macos-")
                    .replace("windows-llvm-mingw-", "windows-"))
    packages = (
        ("salts-base", "SALTS_BASE_VERSION", "SALTS_BASE_ROOT", profile),
        ("cmeta", "CMETA_VERSION", "CMETA_ROOT", profile),
        ("salts-cnet", "SALTS_CORE_VERSION", "SALTSCORE_ROOT", cnet_profile),
        ("salts-utils", "SALTS_UTILS_VERSION", "SALTS_UTILS_ROOT", profile),
        ("chttp", "CHTTP_VERSION", "CHTTP_ROOT", profile),
    )
    libs = [str(installed / triplet / "lib")]
    bins = [str(installed / triplet / "bin")]
    for repo, version_key, root_key, sdk_profile in packages:
        version = os.environ[version_key]
        release = api(f"repos/qigao/{repo}/releases/tags/v{version}")
        if release["draft"] or release["tag_name"] != f"v{version}":
            raise RuntimeError(f"Invalid release identity for {repo} {version}")
        name = f"{repo}-{version}-{sdk_profile}.tar"
        sdk = asset(release, name)
        metadata = asset(release, f"{repo}-{version}-manifest.json")
        manifest_file = stage / metadata["name"]
        download(helper, metadata, manifest_file)
        manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
        if manifest["version"] != version or manifest["repository"] not in (
                f"qigao/{repo}", f"https://github.com/qigao/{repo}"):
            raise RuntimeError(f"Invalid SDK manifest identity for {repo}")
        run_id = manifest.get("producer_run") or manifest.get("ci", {}).get("runId")
        if not run_id:
            raise RuntimeError(f"No producer run in {repo} manifest")
        run = api(f"repos/qigao/{repo}/actions/runs/{run_id}")
        if run["conclusion"] != "success":
            raise RuntimeError(f"SDK producer not successful: {run['html_url']}")
        records = [record for record in manifest["sdks"]
                   if record.get("asset", record.get("file")) == name]
        if len(records) != 1 or records[0]["sha256"] != sdk["digest"][7:]:
            raise RuntimeError(f"Release/manifest SHA-256 mismatch: {name}")
        size = records[0].get("sizeBytes", sdk["size"])
        if size != sdk["size"]:
            raise RuntimeError(f"Release/manifest size mismatch: {name}")
        archive = stage / name
        download(helper, sdk, archive)
        dest = BUILD / "ci-sdk" / repo
        dest.mkdir(parents=True, exist_ok=False)
        with tarfile.open(archive) as tar:
            tar.extractall(dest, filter="data")
        emit(root_key, dest.as_posix())
        if repo == "salts-base":
            emit("LLVM_MINGW_ARM64_SALTS_BASE_ROOT", dest.as_posix())
        if repo == "salts-cnet":
            emit("SALTS_CNET_ROOT", dest.as_posix())
        libs.append(str(dest / "lib"))
        bins.append(str(dest / "bin"))
        print(f"Verified {name}: {sdk['digest']}; {run['html_url']}", flush=True)
    for directory in bins + libs:
        with open(os.environ["GITHUB_PATH"], "a", encoding="utf-8") as stream:
            stream.write(directory + "\n")
    if os.name != "nt":
        for key in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
            emit(key, os.pathsep.join(libs + ([os.environ[key]] if os.environ.get(key) else [])))

if __name__ == "__main__":
    {"pins": pins, "restore": restore}[sys.argv[1]]()
