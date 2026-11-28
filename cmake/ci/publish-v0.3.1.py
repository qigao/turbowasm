"""One-time authorized publication from the already-qualified master commit."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import zipfile

REPO = "qigao/turbowasm"
VERSION = "0.3.1"
TAG = "v" + VERSION
SOURCE = "68df0b0ba2568f1c85b7194a31d9bbb4c154eaf2"
TREE = "c53cbb01da8afe933272b29ae1910553f671b5f9"
SPEC_SOURCE = "f1441fd19e32f367740a6ffc4932bb503d578224"
NATIVE = 46138176740
SPEC = 46137580110
FIXTURE = 46138176613
PROFILES = ("linux-clang-x64", "linux-clang-arm64", "macos-clang-arm64",
            "windows-llvm-mingw-x64", "windows-llvm-mingw-arm64",
            "android-x64", "android-arm64")
ROOT = Path("qualified")
DIST = Path("dist")
DIST.mkdir()
DOWNLOADS = Path("release-inputs")
DOWNLOADS.mkdir()
API = "https://api.github.com/repos/" + REPO
TOKEN = os.environ["GH_TOKEN"]

def curl(*args):
    return subprocess.run(["curl", "--silent", "--show-error", "--location",
                           "--header", "Authorization: Bearer " + TOKEN,
                           "--header", "Accept: application/vnd.github+json",
                           "--header", "X-GitHub-Api-Version: 2022-11-28",
                           *args], check=True, stdout=subprocess.PIPE).stdout

def api(path):
    return json.loads(curl("--fail", API + path))

def absent(path):
    code = curl("--output", str(DOWNLOADS / "response.json"),
                "--write-out", "%{http_code}", API + path).decode()
    if code != "404":
        raise RuntimeError(f"Expected absent release ref, got HTTP {code}: {path}")

def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()

def qualify(run_id, sha, count, expected_names=None):
    run = api(f"/actions/runs/{run_id}")
    if (run["head_sha"], run["status"], run["conclusion"]) != (sha, "completed", "success"):
        raise RuntimeError(f"Unqualified producer: {run_id}")
    jobs = api(f"/actions/runs/{run_id}/jobs?per_page=100")["jobs"]
    if len(jobs) != count or any(j["conclusion"] != "success" for j in jobs):
        raise RuntimeError(f"Invalid producer jobs: {run_id}")
    if expected_names is not None and {j["name"] for j in jobs} != expected_names:
        raise RuntimeError("Native producer job set does not match release matrix")
    return run

absent("/git/ref/tags/" + TAG)
if any(r["tag_name"] == TAG for r in api("/releases?per_page=100")):
    raise RuntimeError("Release or draft already exists; do not overwrite it")
native = qualify(NATIVE, SOURCE, 8, {p + " / Build and qualify" for p in PROFILES} | {"CI result"})
if native["head_branch"] != "master" or native["event"] != "push":
    raise RuntimeError("Native producer is not the merged master push")
qualify(SPEC, SPEC_SOURCE, 13)
qualify(FIXTURE, SOURCE, 1)
for sha in (SOURCE, SPEC_SOURCE):
    if api("/git/commits/" + sha)["tree"]["sha"] != TREE:
        raise RuntimeError("Qualified source tree mismatch")
if subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip() != SOURCE:
    raise RuntimeError("Wrong source checkout")

lock = json.loads((ROOT / "cmake/ci/sdk-lock.json").read_text(encoding="utf-8"))
if set(lock["profiles"]) != set(PROFILES):
    raise RuntimeError("Release matrix and SDK lock disagree")
artifacts = api(f"/actions/runs/{NATIVE}/artifacts?per_page=100")["artifacts"]
expected = {f"turbowasm-sdk-{p}-{SOURCE}": p for p in PROFILES}
selected = {a["name"]: a for a in artifacts if a["name"].startswith("turbowasm-sdk-")}
if set(selected) != set(expected):
    raise RuntimeError("Producer SDK artifacts do not match release matrix")

release = {"version": VERSION, "tag": TAG, "source": SOURCE, "tree": TREE,
           "native_ci": NATIVE, "spec_ci": SPEC, "spec_source": SPEC_SOURCE,
           "fixture_ci": FIXTURE, "dependencies": {"vcpkg-cache": lock["cache"],
               **{p["repo"]: p["version"] for p in lock["packages"]}},
           "profiles": []}

def add_bytes(z, name, data, executable=False):
    info = zipfile.ZipInfo(name)
    info.create_system = 3
    info.external_attr = (0o100755 if executable else 0o100644) << 16
    z.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)

required = {"include/turbowasm.h", "include/component.h",
            "include/turbowasm_simd.h", "include/turbowasm_native_io.h",
            "lib/libturbowasm.a", "lib/cmake/TurboWasm/TurboWasmConfig.cmake",
            "lib/cmake/TurboWasm/TurboWasmTargets.cmake"}

for name, profile in expected.items():
    a = selected[name]
    if a["expired"] or not a["digest"].startswith("sha256:"):
        raise RuntimeError(f"Unavailable or unverified SDK artifact: {name}")
    original = DOWNLOADS / (profile + ".zip")
    curl("--fail", "--output", str(original), API + f"/actions/artifacts/{a['id']}/zip")
    if digest(original) != a["digest"].split(":", 1)[1] or original.stat().st_size != a["size_in_bytes"]:
        raise RuntimeError(f"Producer archive identity mismatch: {name}")
    filename = f"TurboWasm-{VERSION}-{profile}.zip"
    sdk = DIST / filename
    receipt = {"profile": profile, "triplet": lock["profiles"][profile]["triplet"],
               "artifact_id": a["id"], "producer_sha256": a["digest"],
               "qualification": "cross-build/install" if profile.startswith("android-") else "native build/CTest/install"}
    with zipfile.ZipFile(original) as src, zipfile.ZipFile(sdk, "w") as out:
        files = [item for item in src.infolist() if not item.is_dir()]
        names = {item.filename for item in files}
        if len(names) != len(files) or not required.issubset(names):
            raise RuntimeError(f"Incomplete SDK payload: {profile}")
        suffix = ".exe" if profile.startswith("windows-") else ""
        if not {"bin/turbowasm" + suffix, "bin/turbowasm-run" + suffix}.issubset(names):
            raise RuntimeError(f"Missing installed command runners: {profile}")
        for item in sorted(files, key=lambda item: item.filename):
            p = PurePosixPath(item.filename)
            if p.is_absolute() or ".." in p.parts or chr(92) in item.filename:
                raise RuntimeError("Invalid producer archive entry")
            add_bytes(out, item.filename, src.read(item), p.parts[0] == "bin")
        manifest = {k: v for k, v in release.items() if k != "profiles"} | receipt
        add_bytes(out, "BUILD-MANIFEST.txt",
                  ("\n".join(f"{k}={json.dumps(v, sort_keys=True)}" for k, v in manifest.items()) + "\n").encode())
        add_bytes(out, "sdk-lock.json", (ROOT / "cmake/ci/sdk-lock.json").read_bytes())
        for doc in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
            add_bytes(out, doc, (ROOT / doc).read_bytes())
    receipt |= {"filename": filename, "sha256": digest(sdk), "size": sdk.stat().st_size}
    release["profiles"].append(receipt)
    print(f"Packaged {filename}: {receipt['sha256']}")

manifest = DIST / "release-manifest.json"
manifest.write_text(json.dumps(release, indent=2) + "\n", encoding="utf-8")
sums = DIST / "SHA256SUMS"
sums.write_text("".join(f"{digest(p)}  {p.name}\n" for p in sorted(DIST.iterdir())), encoding="utf-8")
notes = DOWNLOADS / "RELEASE-NOTES.md"
notes.write_text("""## Changes

- Restore complete native build and test graphs against exact released split SDKs.
- Qualify Linux x64/ARM64, Windows LLVM-MinGW x64/ARM64, macOS ARM64, and Android x64/ARM64.
- Correct relocated runtime, SIMD, NativeIO and WASI02 headers, MIR dependency propagation, and the Android API 26 realtime clock.
- All 13 specification jobs (including MIR and Core 3.0 MIR) and WASI02 fixture generation passed.

## Downloads

Seven native SDK archives contain the installed headers, static libraries, CMake package exports and command tools. They require matching external SDKs from the pinned dependency family; they are not standalone runtime bundles. See each archive's sdk-lock.json and BUILD-MANIFEST.txt, and verify SHA256SUMS before use.

Android is cross-build/install only. Windows ARM64 and Android use interpreter profiles; MIR is not claimed for those configurations. Metallic guest SDKs and Intel macOS are outside this release's qualification. Linux profiles include the littlefs adapter; other profiles do not.

## Provenance

Source: 68df0b0ba2568f1c85b7194a31d9bbb4c154eaf2

- Native producer: https://github.com/qigao/turbowasm/actions/runs/46138176740 (7 platform jobs plus aggregate gate).
- Specification conformance: https://github.com/qigao/turbowasm/actions/runs/46137580110 (identical source tree).
- WASI02 fixtures: https://github.com/qigao/turbowasm/actions/runs/46138176613.

The exact package provenance is in release-manifest.json.
""", encoding="utf-8")

# Draft first, then verify every asset against its local digest before making it public.
files = sorted(DIST.iterdir())
subprocess.run(["gh", "release", "create", TAG, *map(str, files),
                "--repo", REPO, "--target", SOURCE, "--draft",
                "--title", "TurboWasm " + VERSION, "--notes-file", str(notes)], check=True)
drafts = [r for r in api("/releases?per_page=100") if r["tag_name"] == TAG]
if len(drafts) != 1 or not drafts[0]["draft"] or drafts[0]["target_commitish"] != SOURCE:
    raise RuntimeError("Draft release provenance mismatch")
draft = drafts[0]
uploaded = {a["name"]: a for a in api(f"/releases/{draft['id']}/assets?per_page=100")}
if set(uploaded) != {p.name for p in files}:
    raise RuntimeError("Draft asset inventory mismatch")
for p in files:
    a = uploaded[p.name]
    if a["state"] != "uploaded" or a["size"] != p.stat().st_size or a["digest"] != "sha256:" + digest(p):
        raise RuntimeError(f"Draft asset digest/size mismatch: {p.name}")
subprocess.run(["gh", "release", "edit", TAG, "--repo", REPO,
                "--draft=false", "--latest"], check=True)
published = api("/releases/tags/" + TAG)
tag = api("/git/ref/tags/" + TAG)["object"]
if tag["type"] == "tag":
    tag = api("/git/tags/" + tag["sha"])["object"]
if published["draft"] or published["prerelease"] or tag["type"] != "commit" or tag["sha"] != SOURCE:
    raise RuntimeError("Published release provenance mismatch")
print(published["html_url"])
