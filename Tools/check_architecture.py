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
#
# Keyed by the tier of the module doing the including; the values are *include-path
# prefixes*, not module names, so a new Tier 1 or Tier 3 module is only policed here once
# its header prefix is added. Which is why Monarc/Host/ appears below: Monarc.Host.Windowed
# is Tier 3, and without its prefix this gate would have had nothing that actually exists
# to forbid -- every other entry names a module Monarc has not written yet.
FORBIDDEN_INCLUDES = {
    2: [
        "Monarc/Reflect/",
        "Monarc/Serialize/",
        "Monarc/Assets/",
        "Monarc/World/",
        "Monarc/Host/",
    ],
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


def gate_app_leaves(modules: dict) -> Gate:
    """Rule 8 of Module-Graph.md: nothing may depend on an app.

    monarc_validate_modules() already refuses this at configure time, which is where a
    developer wants to hear about it. This checks the same rule against the emitted
    module-graph.json, which is a different thing worth checking: that file is the product
    `monarc explain` reads, so a stale, hand-edited or otherwise unvalidated graph would
    otherwise answer "why is this in the export" from a shape CMake never approved.
    """
    gate = Gate(12, "apps are graph leaves (nothing depends on an app)")
    for name, mod in modules.items():
        for dep in mod["publicDeps"] + mod["privateDeps"]:
            target = modules.get(dep)
            # An edge pointing outside the graph is CMake's to reject, not this gate's --
            # the same division of labour gate 2 uses.
            if target is not None and target.get("app", False):
                gate.fail(f"{name} depends on {dep}, which is an app")
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
    # 13, not 7. This gate was numbered 7 from A1 until A3, which collided with M0's own
    # gate 7 (world kinds) -- so ctest printed "Gate 7: ... layout" while the milestone's
    # gate 7 was about something else entirely, and anyone cross-referencing the two got
    # the wrong rule. M0's verification-gates table is the authority for these numbers;
    # the layout rule was simply missing from it. Corrected in A3 Task 1, which rewrote
    # this gate's semantics anyway.
    gate = Gate(13, "every module has Private/; libraries have Include/ and apps do not")
    for name, mod in modules.items():
        base = root / mod["directory"]
        # An app is a link target, not an interface: nothing may depend on it, so it has no
        # public headers. Note the direction -- an app is not merely excused from having
        # Include/, it is forbidden one. An exemption alone would leave a trap: monarc_app()
        # does not glob an app's Include/, so nothing in the build ever reads a header put
        # there, while the source-reading gates go on scanning and policing it: iter_sources
        # walks Include/ for every module, app or not, so gate 10 reads such a header always
        # and gate 3 reads it whenever the app is tier 2. Either way it is a file that is
        # simultaneously governed and dead. A rule that says which of the two is right has no
        # such gap, and it costs nothing to state.
        #
        # `.get` rather than `[...]`: a module-graph.json written before the key existed
        # still loads, and so do the hand-built graphs in test_check_architecture.py.
        has_include = (base / "Include").is_dir()
        if mod.get("app", False):
            if has_include:
                gate.fail(f"{name} is an app and must not have {mod['directory']}/Include")
        elif not has_include:
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

    # Reported in gate-number order, so the output reads as a checklist rather than as
    # whatever order the functions happen to be defined in. Kept by writing the list in that
    # order rather than sorting at run time: the numbers are literals a few lines up in this
    # same file, and a sort would be machinery standing in for reading them.
    gates = [
        gate_acyclic(modules),  # 2
        gate_package_boundary(modules, root),  # 3
        gate_platform_containment(modules, root),  # 10
        gate_app_leaves(modules),  # 12
        gate_layout(modules, root),  # 13
    ]

    results = [g.report() for g in gates]
    passed = all(results)
    print("all gates passed" if passed else "ARCHITECTURE GATES FAILED")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
