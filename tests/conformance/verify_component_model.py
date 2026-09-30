#!/usr/bin/env python3
import argparse
import json
import pathlib
import subprocess
import sys


def git(root: pathlib.Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), *args],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return result.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--root", required=True)
    args = parser.parse_args()

    manifest_path = pathlib.Path(args.manifest)
    root = pathlib.Path(args.root)
    data = json.loads(manifest_path.read_text(encoding="utf-8"))

    actual_commit = git(root, "rev-parse", "HEAD")
    if actual_commit != data["commit"]:
        print(
            f"component-model commit mismatch: "
            f"expected={data['commit']} actual={actual_commit}",
            file=sys.stderr,
        )
        return 1

    for relative, spec in data["files"].items():
        path = root / relative
        if not path.is_file():
            print(f"missing pinned upstream file: {relative}", file=sys.stderr)
            return 1

        actual_blob = git(root, "hash-object", relative)
        expected_blob = spec["git_blob_sha1"]
        if actual_blob != expected_blob:
            print(
                f"blob mismatch for {relative}: "
                f"expected={expected_blob} actual={actual_blob}",
                file=sys.stderr,
            )
            return 1

        text = path.read_text(encoding="utf-8")
        for anchor in spec.get("contains", []):
            if anchor not in text:
                print(
                    f"missing pinned anchor in {relative}: {anchor!r}",
                    file=sys.stderr,
                )
                return 1

        print(f"COMPONENT_MODEL_PIN_OK file={relative} blob={actual_blob}")

    print(
        "COMPONENT_MODEL_PIN_OK "
        f"commit={actual_commit} files={len(data['files'])}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
