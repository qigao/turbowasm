"""Build Metallic SDK archives and guest modules for Slate's Ninja graph."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


GUEST = Path(__file__).resolve().parent
TARGET = ["--target=wasm32-unknown-unknown", "-mbulk-memory"]
SJLJ = ["-mexception-handling", "-mllvm", "-wasm-enable-sjlj",
        "-mllvm", "-wasm-use-legacy-eh=false"]
THREADS = ["-D__METALLIC_THREADS__=1", "-matomics", "-pthread", "-ftls-model=local-exec"]
LINK_SJLJ = [
    "-mexception-handling",
    "-Wl,-mllvm,-wasm-enable-sjlj,-mllvm,-wasm-use-legacy-eh=false,-mllvm,-exception-model=wasm",
    "-Wl,-u,__wasm_setjmp,-u,__wasm_setjmp_test,-u,__wasm_longjmp",
]


def run(*command):
    subprocess.run(command, check=True)


def compile_object(clang, source, output, options, *, vendor=False):
    standard = "-std=gnu11" if vendor else "-std=c11"
    run(clang, *TARGET, *SJLJ, "-O2", "-flto", standard,
        "-I", GUEST / "include", "-I", GUEST / "metallic/include",
        *options, "-c", source, "-o", output)


def archive(ar, output, objects):
    # LLVM's response file keeps the archive command below Windows' length limit.
    response = output.with_suffix(".rsp")
    response.write_text("".join(f'"{path.as_posix()}"\n' for path in objects),
                        encoding="utf-8")
    run(ar, "rcs", output, f"@{response}")


def build_sdk(clang, ar, output):
    manifest = json.loads((GUEST / "module.json").read_text(encoding="utf-8"))
    task, = manifest["targets"]["turbowasm_guest_sdk"]["preTasks"]
    sources = [Path(path) for path in task["inputs"]
               if Path(path).suffix in (".c", ".S")]
    crt_names = {"crt1.c": "crt1.o", "crt1-reactor.c": "crt1-reactor.o"}
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output) as temporary:
        temp = Path(temporary)
        for profile, flags in (("single", ["-D__STDC_NO_THREADS__=1"]),
                               ("threaded", THREADS)):
            profile_dir = temp / profile
            profile_dir.mkdir()
            objects = []
            for source in sources:
                if profile == "single" and source.parent.name == "threads":
                    continue
                crt = crt_names.get(source.name)
                obj = profile_dir / (crt or f"{len(objects)}.o")
                # Late Wasm SJLJ lowering references these unrenamed definitions.
                options = [*flags, "-fno-lto"] if source.parent.name == "setjmp" else flags
                compile_object(clang, GUEST / source, obj, options, vendor=True)
                if not crt:
                    objects.append(obj)
            filename = "metallic.a" if profile == "single" else "metallic-threaded.a"
            archive(ar, profile_dir / filename, objects)
        for profile in ("single", "threaded"):
            destination = output / profile
            destination.mkdir(exist_ok=True)
            for candidate in (temp / profile).iterdir():
                if candidate.suffix in (".a", ".o") and not candidate.stem.isdigit():
                    candidate.replace(destination / candidate.name)


def build_guest(clang, ar, args):
    args.output.parent.mkdir(parents=True, exist_ok=True)
    flags = THREADS if args.threaded else ["-D__STDC_NO_THREADS__=1"]
    includes = [item for directory in args.include for item in ("-I", directory)]
    options = [*flags, *includes, *args.compile_option]
    with tempfile.TemporaryDirectory(dir=args.output.parent) as temporary:
        temp = Path(temporary)
        objects = []
        for i, source in enumerate(args.source):
            obj = temp / f"{i}.o"
            compile_object(clang, source, obj, options)
            objects.append(obj)
        output = temp / args.output.name
        if args.kind == "archive":
            archive(ar, output, objects)
        else:
            link_flags = [*TARGET, "-O2", "-flto", "-nostdlib", *LINK_SJLJ,
                          "-Wl,--stack-first,-z,stack-size=262144",
                          f"-Wl,--max-memory={args.max_memory}"]
            exports = list(args.export)
            if args.threaded:
                link_flags += ["-matomics", "-pthread",
                               "-Wl,--shared-memory,--import-memory,--initial-memory=1048576"]
                exports.append("wasi_thread_start")
            if args.kind in ("reactor", "bare"):
                link_flags.append("-Wl,--no-entry")
            if args.kind == "reactor":
                exports.append("_initialize")
            crt = [args.crt] if args.crt else []
            run(clang, *link_flags, *objects, *crt, *args.library,
                *(f"-Wl,--export={name}" for name in exports), "-o", output)
        output.replace(args.output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="mode", required=True)
    sdk = subcommands.add_parser("sdk")
    sdk.add_argument("--output", type=Path, required=True)
    guest = subcommands.add_parser("guest")
    guest.add_argument("--output", type=Path, required=True)
    guest.add_argument("--kind", choices=("command", "reactor", "bare", "archive"),
                       required=True)
    guest.add_argument("--source", type=Path, action="append", required=True)
    guest.add_argument("--include", type=Path, action="append", default=[])
    guest.add_argument("--library", type=Path, action="append", default=[])
    guest.add_argument("--export", action="append", default=[])
    guest.add_argument("--compile-option", action="append", default=[])
    guest.add_argument("--crt", type=Path)
    guest.add_argument("--threaded", action="store_true")
    guest.add_argument("--max-memory", type=int, default=16777216)
    args = parser.parse_args()

    clang = shutil.which("clang")
    ar = shutil.which("llvm-ar")
    if not clang or not ar:
        parser.error("Clang and llvm-ar must be on PATH")
    clang_version = subprocess.check_output([clang, "--version"], text=True)
    version = re.search(r"clang version (\d+)", clang_version)
    if not version or int(version[1]) < 20:
        parser.error("Metallic needs LLVM 20 or later for Wasm EH and SJLJ")

    if args.mode == "sdk":
        build_sdk(clang, ar, args.output)
    else:
        build_guest(clang, ar, args)


if __name__ == "__main__":
    main()
