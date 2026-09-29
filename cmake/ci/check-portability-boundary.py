#!/usr/bin/env python3
"""Keep the installed TurboWasm Runtime core free of direct OS API coupling."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CMAKE = ROOT / "CMakeLists.txt"

FORBIDDEN_HEADERS = (
    "windows.h",
    "winsock2.h",
    "ws2tcpip.h",
    "pthread.h",
    "unistd.h",
    "fcntl.h",
    "sys/mman.h",
    "sys/socket.h",
    "sys/epoll.h",
    "sys/event.h",
    "liburing.h",
)

FORBIDDEN_PATTERNS = (
    (r"\bVirtualAlloc\s*\(", "VirtualAlloc"),
    (r"\bVirtualFree\s*\(", "VirtualFree"),
    (r"\bCreateThread\s*\(", "CreateThread"),
    (r"\bWaitForSingleObject\s*\(", "WaitForSingleObject"),
    (r"\bpthread_[A-Za-z0-9_]+\s*\(", "pthread API"),
    (r"\bmmap\s*\(", "mmap"),
    (r"\bmunmap\s*\(", "munmap"),
    (r"\bsocket\s*\(", "socket"),
    (r"\bepoll_[A-Za-z0-9_]+\s*\(", "epoll API"),
    (r"\bkqueue\s*\(", "kqueue"),
    (r"\bkevent\s*\(", "kevent"),
    (r"\bio_uring_[A-Za-z0-9_]+\s*\(", "io_uring API"),
    (r"(?<![A-Za-z0-9_])malloc\s*\(", "direct malloc"),
    (r"(?<![A-Za-z0-9_])calloc\s*\(", "direct calloc"),
    (r"(?<![A-Za-z0-9_])realloc\s*\(", "direct realloc"),
    (r"(?<![A-Za-z0-9_])free\s*\(", "direct free"),
)


def runtime_sources() -> list[Path]:
    text = CMAKE.read_text(encoding="utf-8")
    match = re.search(
        r"add_library\(turbowasm\s+(.*?)\)\s*\n\s*"
        r"add_library\(TurboWasm::Runtime\s+ALIAS\s+turbowasm\)",
        text,
        flags=re.DOTALL,
    )
    if match is None:
        raise RuntimeError("cannot locate canonical turbowasm Runtime source list")

    sources: list[Path] = []
    for token in re.findall(r"[^\s]+", match.group(1)):
        token = token.strip('"')
        if token.startswith("$"):
            raise RuntimeError(
                f"Runtime source list contains unresolved expression: {token}"
            )
        path = ROOT / token
        if not path.is_file():
            raise RuntimeError(f"Runtime source is missing: {token}")
        sources.append(path)
    if not sources:
        raise RuntimeError("Runtime source list is empty")
    return sources


def main() -> int:
    failures: list[str] = []

    try:
        sources = runtime_sources()
    except RuntimeError as exc:
        print(f"Runtime portability guard: {exc}", file=sys.stderr)
        return 1

    for path in sources:
        text = path.read_text(encoding="utf-8")
        rel = path.relative_to(ROOT)

        for header in FORBIDDEN_HEADERS:
            if re.search(
                rf"#\s*include\s*[<\"]{re.escape(header)}[>\"]",
                text,
            ):
                failures.append(f"{rel}: direct platform header <{header}>")

        for pattern, label in FORBIDDEN_PATTERNS:
            if (rel == Path("src/runtime_alloc.c") and
                    label.startswith("direct ")):
                continue
            if re.search(pattern, text):
                failures.append(f"{rel}: forbidden Runtime API {label}")

    if failures:
        print("Runtime portability boundary violations:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print(
            "Route platform services through Salts or an optional adapter target.",
            file=sys.stderr,
        )
        return 1

    print(
        "Runtime portability boundary: OK "
        f"({len(sources)} canonical Runtime source files)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
