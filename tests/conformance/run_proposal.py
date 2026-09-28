#!/usr/bin/env python3
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

REQUIRED_SELECTORS = {
    "exception-handling",
    "threads",
    "tail-call",
    "multi-memory",
    "custom-page-sizes",
    "extended-const",
    "relaxed-simd",
}
SHA_RE = re.compile(r"^[0-9a-f]{40}$")


def load_suite(path):
    entries = []
    seen = set()
    with open(path, "r", encoding="utf-8") as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line in seen:
                raise ValueError(f"duplicate suite entry {line!r} in {path}")
            seen.add(line)
            entries.append(line)
    if not entries:
        raise ValueError(f"empty proposal suite: {path}")
    return entries


def load_config(path):
    with open(path, "r", encoding="utf-8") as handle:
        config = json.load(handle)
    if not isinstance(config, dict):
        raise ValueError("proposal config must be an object")
    return config


def validate_selector(name, selector, config_dir):
    if not isinstance(selector, dict):
        raise ValueError(f"{name}: selector must be an object")

    repository = selector.get("repository")
    commit = selector.get("commit")
    core_dir = selector.get("core_dir")
    suite_name = selector.get("suite")
    flags = selector.get("wast2json_flags")
    minimum_passes = selector.get("minimum_passes")

    if not isinstance(repository, str) or not repository.startswith(
        "https://github.com/WebAssembly/"
    ) or not repository.endswith(".git"):
        raise ValueError(f"{name}: invalid upstream repository")
    if not isinstance(commit, str) or SHA_RE.fullmatch(commit) is None:
        raise ValueError(f"{name}: commit must be an exact 40-hex SHA")
    if not isinstance(core_dir, str) or not core_dir or os.path.isabs(core_dir):
        raise ValueError(f"{name}: invalid relative core_dir")
    if not isinstance(suite_name, str) or not suite_name.endswith(".txt"):
        raise ValueError(f"{name}: invalid suite file")
    if not isinstance(flags, list) or not all(
        isinstance(flag, str) and flag.startswith("--")
        for flag in flags
    ):
        raise ValueError(f"{name}: wast2json_flags must be option strings")
    if not isinstance(minimum_passes, int) or minimum_passes < 0:
        raise ValueError(f"{name}: minimum_passes must be non-negative")

    suite_path = os.path.join(config_dir, suite_name)
    if not os.path.isfile(suite_path):
        raise ValueError(f"{name}: suite file not found: {suite_path}")

    entries = load_suite(suite_path)
    return suite_path, entries


def validate_all(config_path):
    config = load_config(config_path)
    names = set(config)
    if names != REQUIRED_SELECTORS:
        missing = sorted(REQUIRED_SELECTORS - names)
        extra = sorted(names - REQUIRED_SELECTORS)
        raise ValueError(
            f"proposal selector set mismatch: missing={missing} extra={extra}"
        )

    config_dir = os.path.dirname(os.path.abspath(config_path))
    for name in sorted(config):
        suite_path, entries = validate_selector(
            name, config[name], config_dir
        )
        print(
            f"PROPOSAL_SELECTOR_VALID name={name} "
            f"files={len(entries)} suite={os.path.basename(suite_path)}"
        )
    return config


def materialize(repository, commit, destination):
    subprocess.run(["git", "init", destination], check=True)
    subprocess.run(
        ["git", "-C", destination, "remote", "add", "origin", repository],
        check=True,
    )
    subprocess.run(
        [
            "git",
            "-C",
            destination,
            "fetch",
            "--depth=1",
            "origin",
            commit,
        ],
        check=True,
    )
    subprocess.run(
        ["git", "-C", destination, "checkout", "--detach", "FETCH_HEAD"],
        check=True,
    )
    resolved = subprocess.check_output(
        ["git", "-C", destination, "rev-parse", "HEAD"],
        text=True,
    ).strip()
    if resolved != commit:
        raise RuntimeError(
            f"upstream SHA mismatch: expected {commit}, got {resolved}"
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--config",
        default=os.path.join(
            os.path.dirname(__file__),
            "proposals.json",
        ),
    )
    parser.add_argument("--validate-all", action="store_true")
    parser.add_argument("--selector", choices=sorted(REQUIRED_SELECTORS))
    parser.add_argument("--wast2json")
    parser.add_argument("--runner")
    parser.add_argument("--work-dir")
    args = parser.parse_args()

    try:
        config = validate_all(args.config)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"proposal selector validation failed: {error}", file=sys.stderr)
        return 2

    if args.validate_all and args.selector is None:
        return 0

    if args.selector is None or args.wast2json is None or args.runner is None:
        parser.error(
            "--selector, --wast2json and --runner are required for execution"
        )

    selector = config[args.selector]
    config_dir = os.path.dirname(os.path.abspath(args.config))
    suite_path, entries = validate_selector(
        args.selector, selector, config_dir
    )

    owned_temp = None
    if args.work_dir is None:
        owned_temp = tempfile.TemporaryDirectory(
            prefix=f"turbowasm-proposal-{args.selector}-"
        )
        work_dir = owned_temp.name
    else:
        work_dir = os.path.abspath(args.work_dir)
        os.makedirs(work_dir, exist_ok=True)

    upstream = os.path.join(work_dir, "upstream")
    if os.path.exists(upstream):
        raise RuntimeError(f"upstream destination already exists: {upstream}")

    materialize(
        selector["repository"],
        selector["commit"],
        upstream,
    )

    core_dir = os.path.join(upstream, selector["core_dir"])
    if not os.path.isdir(core_dir):
        raise RuntimeError(f"proposal core_dir missing: {core_dir}")

    missing = [
        entry for entry in entries
        if not os.path.isfile(os.path.join(core_dir, entry))
    ]
    if missing:
        raise RuntimeError(
            f"proposal suite files missing at pinned SHA: {missing}"
        )

    print(
        f"PROPOSAL_SELECTOR name={args.selector} "
        f"commit={selector['commit']} files={len(entries)}"
    )

    run_core = os.path.join(config_dir, "run_core.py")
    command = [
        sys.executable,
        run_core,
        "--wast2json",
        args.wast2json,
        "--runner",
        args.runner,
        "--core-dir",
        core_dir,
        "--suite",
        suite_path,
        "--minimum-passes",
        str(selector["minimum_passes"]),
    ]
    for flag in selector["wast2json_flags"]:
        command.append(f"--wast2json-flag={flag}")

    completed = subprocess.run(command, check=False)
    if owned_temp is not None:
        owned_temp.cleanup()
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
