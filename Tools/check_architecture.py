#!/usr/bin/env python3
"""Monarc architecture gates.

Checks the structural rules that CMake cannot check directly. Run by CTest, so a
violation fails a test run rather than merely being noticed in review.

Usage:  check_architecture.py <module-graph.json> <repo-root>
"""
from __future__ import annotations

import json
import pathlib
import re
import sys

SOURCE_SUFFIXES = {".h", ".hpp", ".inl", ".cpp"}

# Rule 4 of Module-Graph.md, generalised: for each tier, the tiers it may NOT
# reference at source level. Tier 2 is the renderer package boundary (ADR-0007).
FORBIDDEN_INCLUDES = {
    2: ["Monarc/Reflect/", "Monarc/Serialize/", "Monarc/Assets/", "Monarc/World/"],
}

# Rule 7: platform-conditional compilation is confined to Core/Platform.
#
# Deliberately excludes _MSC_VER and __clang__. Those are *compiler* macros, not platform
# macros, and ADR-0012's rule is about platform portability. Compiler differences are
# legitimate anywhere -- that is what building with two compilers under /WX is for.
PLATFORM_MACROS = re.compile(
    r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*\b("
    r"_WIN32|_WIN64|__APPLE__|__linux__|__ANDROID__|TARGET_OS_[A-Z]+"
    r")\b"
)
# Trailing slashes are load-bearing: without them a bare prefix match would also exempt
# a sibling such as Private/PlatformUtils/, which is a different directory and must not be.
# The only place a platform conditional is legitimate is inside a per-platform source
# directory -- Private/Platform/<Platform>/. ADR-0016 keeps public headers platform-neutral
# and puts platform code in per-platform directories, so this is deliberately narrow:
#
#   Private/Platform/Windows/File.cpp   exempt -- genuinely one platform's code
#   Private/Platform/Path.cpp           NOT exempt -- lives beside them but is neutral
#   Include/.../Platform/Path.h         NOT exempt -- public headers are always neutral
#
# Exempting the whole Private/Platform/ tree, as this once did, would let a Windows
# conditional sit unnoticed in a file explicitly labelled platform-neutral.
PLATFORM_EXEMPT = re.compile(r"(?:^|/)Private/Platform/[^/]+/")


class Gate:
    def __init__(self, number: int, name: str) -> None:
        self.number, self.name, self.failures = number, name, []

    def fail(self, message: str) -> None:
        self.failures.append(message)

    def report(self) -> bool:
        status = "PASS" if not self.failures else "FAIL"
        print(f"[{status}] Gate {self.number}: {self.name}")
        for f in self.failures:
            print(f"         {f}")
        return not self.failures


def logical_lines(text: str):
    """Yield (line_number, joined_text) with backslash continuations merged.

    A preprocessor conditional split across lines for readability, e.g.

        #if defined(FEATURE) || \\
            defined(_WIN32)

    would otherwise defeat a single-line regex: the first line carries the `#if` but
    no platform token, and the second carries the token but no `#`. Merging first
    means the gate sees one logical directive.
    """
    pending: list[str] = []
    start = 1
    for lineno, line in enumerate(text.splitlines(), 1):
        if not pending:
            start = lineno
        stripped = line.rstrip()
        if stripped.endswith("\\"):
            pending.append(stripped[:-1])
            continue
        pending.append(stripped)
        yield start, " ".join(part.strip() for part in pending)
        pending = []
    if pending:
        yield start, " ".join(part.strip() for part in pending)


# A module's own code lives in these subdirectories. Tests/ is deliberately excluded:
# test targets are not modules, nothing may depend on them, and they are entitled to do
# things module code may not -- exercise a platform #ifdef, or reach across a tier to
# test a boundary. Scanning them would apply module rules to code outside the graph.
MODULE_SOURCE_DIRS = ("Include", "Private")


def iter_sources(root: pathlib.Path, module_dir: str):
    base = root / module_dir
    if not base.is_dir():
        return
    for subdir in MODULE_SOURCE_DIRS:
        source_root = base / subdir
        if not source_root.is_dir():
            continue
        for path in source_root.rglob("*"):
            if path.suffix in SOURCE_SUFFIXES and path.is_file():
                yield path


def gate_acyclic(modules: dict) -> Gate:
    gate = Gate(2, "module graph is acyclic")
    colour: dict[str, int] = {}          # 0 = visiting, 1 = done

    def visit(name: str, stack: list[str]) -> None:
        state = colour.get(name)
        if state == 1:
            return
        if state == 0:
            cycle = " -> ".join(stack[stack.index(name):] + [name])
            gate.fail(f"cycle: {cycle}")
            return
        colour[name] = 0
        for dep in modules[name]["publicDeps"] + modules[name]["privateDeps"]:
            if dep in modules:
                visit(dep, stack + [name])
        colour[name] = 1

    for name in modules:
        visit(name, [])
    return gate


def gate_package_boundary(modules: dict, root: pathlib.Path) -> Gate:
    gate = Gate(3, "renderer package boundary (tier 2 sees no tier 1 or 3)")
    for name, mod in modules.items():
        forbidden = FORBIDDEN_INCLUDES.get(mod["tier"])
        if not forbidden:
            continue
        for path in iter_sources(root, mod["directory"]):
            for lineno, line in enumerate(
                    path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                if not line.lstrip().startswith("#include"):
                    continue
                for bad in forbidden:
                    if bad in line:
                        rel = path.relative_to(root).as_posix()
                        gate.fail(f"{rel}:{lineno} includes {bad} (module {name})")
    return gate


def gate_platform_containment(modules: dict, root: pathlib.Path) -> Gate:
    gate = Gate(10, "platform conditionals only in Core/Platform")
    for mod in modules.values():
        for path in iter_sources(root, mod["directory"]):
            rel = path.relative_to(root).as_posix()
            if PLATFORM_EXEMPT.search(rel):
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for lineno, line in logical_lines(text):
                if PLATFORM_MACROS.match(line):
                    gate.fail(f"{rel}:{lineno} {line.strip()}")
    return gate


def gate_layout(modules: dict, root: pathlib.Path) -> Gate:
    gate = Gate(7, "every module follows the Include/Private layout")
    for name, mod in modules.items():
        base = root / mod["directory"]
        if not (base / "Include").is_dir():
            gate.fail(f"{name}: missing {mod['directory']}/Include")
        if not (base / "Private").is_dir():
            gate.fail(f"{name}: missing {mod['directory']}/Private")
    return gate


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    graph_path, root = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]).resolve()
    graph = json.loads(graph_path.read_text(encoding="utf-8"))
    modules = {m["name"]: m for m in graph["modules"]}

    print(f"Monarc architecture gates | {len(modules)} module(s) | engine {graph['engineVersion']}")

    gates = [
        gate_acyclic(modules),
        gate_package_boundary(modules, root),
        gate_layout(modules, root),
        gate_platform_containment(modules, root),
    ]

    results = [g.report() for g in gates]
    passed = all(results)
    print("all gates passed" if passed else "ARCHITECTURE GATES FAILED")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
