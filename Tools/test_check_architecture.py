#!/usr/bin/env python3
"""Tests for the architecture gates.

`check_architecture.py` is the mechanism Monarc's module rules actually rest on beyond what
CMake can check, and it is string-matching over source trees -- the kind of code that
regresses silently. It has already needed two non-obvious fixes: a bare prefix match that
would have exempted a sibling directory whose name merely started with "Platform", and a
single-line regex defeated by a backslash-continued `#if`. Both have a test here.

Every case builds a real directory tree containing a real violation and asserts the gate
reports it, so the suite verifies the gates *fire* rather than merely that they run.

Standard library only, deliberately: three Python interpreters exist on the development
machine and CI uses a fourth, so which one runs this depends on how it is invoked.

Run directly, or via `ctest` as the Architecture.GateTests test.
"""
from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import check_architecture as ca  # noqa: E402


def module(name, tier=0, kind="Runtime", directory="M", public=None, private=None):
    return {
        "name": name,
        "kind": kind,
        "tier": tier,
        "directory": directory,
        "publicDeps": public or [],
        "privateDeps": private or [],
    }


def graph(*modules):
    return {m["name"]: m for m in modules}


#: A minimal source file carrying a platform conditional.
WIN32_IFDEF = "#ifdef _WIN32\nint w = 1;\n#endif\n"


class TreeFixture:
    """A throwaway source tree. Paths are relative to the tree root."""

    def __init__(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)

    def write(self, relative_path, text):
        path = self.root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def mkdir(self, relative_path):
        (self.root / relative_path).mkdir(parents=True, exist_ok=True)

    def close(self):
        self._tmp.cleanup()


class FixtureTest(unittest.TestCase):
    def setUp(self):
        self.tree = TreeFixture()
        self.addCleanup(self.tree.close)

    def assertGateFails(self, gate, expected_substring=None):
        self.assertTrue(gate.failures, f"gate {gate.number} should have failed but passed")
        if expected_substring is not None:
            joined = " | ".join(gate.failures)
            self.assertIn(expected_substring, joined)

    def assertGatePasses(self, gate):
        self.assertFalse(
            gate.failures, f"gate {gate.number} should have passed; got {gate.failures}"
        )


class TestAcyclic(FixtureTest):
    def test_a_linear_chain_is_acyclic(self):
        g = graph(module("A", public=["B"]), module("B", public=["C"]), module("C"))
        self.assertGatePasses(ca.gate_acyclic(g))

    def test_a_diamond_is_not_a_cycle(self):
        # A depends on B and C, both of which depend on D. Revisiting D must not be
        # mistaken for a cycle -- a naive "already seen" check gets this wrong.
        g = graph(
            module("A", public=["B", "C"]),
            module("B", public=["D"]),
            module("C", public=["D"]),
            module("D"),
        )
        self.assertGatePasses(ca.gate_acyclic(g))

    def test_a_two_module_cycle_is_detected(self):
        g = graph(module("X", public=["Y"]), module("Y", public=["X"]))
        self.assertGateFails(ca.gate_acyclic(g), "cycle")

    def test_a_three_module_cycle_is_detected(self):
        g = graph(
            module("X", public=["Y"]), module("Y", public=["Z"]), module("Z", public=["X"])
        )
        self.assertGateFails(ca.gate_acyclic(g), "cycle")

    def test_a_self_dependency_is_detected(self):
        self.assertGateFails(ca.gate_acyclic(graph(module("X", public=["X"]))), "cycle")

    def test_a_cycle_reached_through_private_deps_is_detected(self):
        # Both dependency kinds are edges. Checking only publicDeps would miss this.
        g = graph(module("X", private=["Y"]), module("Y", private=["X"]))
        self.assertGateFails(ca.gate_acyclic(g), "cycle")

    def test_a_dependency_on_an_undeclared_module_is_ignored_here(self):
        # CMake rejects unknown dependencies at configure time; this gate only cares about
        # cycles, and must not crash on an edge pointing outside the graph.
        self.assertGatePasses(ca.gate_acyclic(graph(module("X", public=["Nonexistent"]))))


class TestPackageBoundary(FixtureTest):
    def _tier2_with_include(self, include_line):
        self.tree.mkdir("R/Include")
        self.tree.write("R/Private/r.cpp", f"{include_line}\nint r = 0;\n")
        return ca.gate_package_boundary(
            graph(module("Monarc.Render", tier=2, directory="R")), self.tree.root
        )

    def test_tier2_including_world_fails(self):
        self.assertGateFails(
            self._tier2_with_include('#include <Monarc/World/Actor.h>'), "Monarc/World/"
        )

    def test_tier2_including_assets_fails(self):
        self.assertGateFails(
            self._tier2_with_include('#include <Monarc/Assets/Handle.h>'), "Monarc/Assets/"
        )

    def test_tier2_including_reflect_fails(self):
        self.assertGateFails(
            self._tier2_with_include('#include <Monarc/Reflect/Type.h>'), "Monarc/Reflect/"
        )

    def test_tier2_including_core_passes(self):
        self.assertGatePasses(self._tier2_with_include('#include <Monarc/Core/Types.h>'))

    def test_a_forbidden_name_in_a_comment_is_not_an_include(self):
        # Only #include lines count. A mention in prose must not fail the gate.
        self.tree.mkdir("R/Include")
        self.tree.write("R/Private/r.cpp", "// Monarc/World/Actor.h is deliberately not used\n")
        self.assertGatePasses(
            ca.gate_package_boundary(
                graph(module("Monarc.Render", tier=2, directory="R")), self.tree.root
            )
        )

    def test_other_tiers_are_unconstrained(self):
        self.tree.mkdir("W/Include")
        self.tree.write("W/Private/w.cpp", '#include <Monarc/Assets/Handle.h>\n')
        self.assertGatePasses(
            ca.gate_package_boundary(
                graph(module("Monarc.World", tier=3, directory="W")), self.tree.root
            )
        )


class TestLayout(FixtureTest):
    def test_both_directories_present_passes(self):
        self.tree.mkdir("M/Include")
        self.tree.mkdir("M/Private")
        self.assertGatePasses(ca.gate_layout(graph(module("M", directory="M")), self.tree.root))

    def test_missing_include_fails(self):
        self.tree.mkdir("M/Private")
        self.assertGateFails(
            ca.gate_layout(graph(module("M", directory="M")), self.tree.root), "Include"
        )

    def test_missing_private_fails(self):
        self.tree.mkdir("M/Include")
        self.assertGateFails(
            ca.gate_layout(graph(module("M", directory="M")), self.tree.root), "Private"
        )


class TestPlatformContainment(FixtureTest):
    def _gate(self):
        return ca.gate_platform_containment(
            graph(module("Monarc.Core", directory="Source/Monarc.Core")), self.tree.root
        )

    def _core(self, relative, text):
        self.tree.mkdir("Source/Monarc.Core/Include")
        self.tree.write(f"Source/Monarc.Core/{relative}", text)

    def test_a_platform_ifdef_outside_platform_fails(self):
        self._core("Private/p.cpp", "#ifdef _WIN32\nint w = 1;\n#endif\n")
        self.assertGateFails(self._gate(), "_WIN32")

    def test_the_same_ifdef_inside_a_platform_directory_is_exempt(self):
        # Note the path: a per-platform directory, not Private/Platform/ itself. This case
        # originally used the latter, which passed only because the exemption was too broad.
        self._core("Private/Platform/Windows/p.cpp", WIN32_IFDEF)
        self.assertGatePasses(self._gate())

    def test_a_platform_sibling_directory_is_not_exempt(self):
        # Regression: the exempt list once matched bare prefixes, so PlatformUtils/ was
        # exempted purely because its name starts with "Platform".
        self._core("Private/PlatformUtils/u.cpp", "#ifdef _WIN32\nint w = 1;\n#endif\n")
        self.assertGateFails(self._gate(), "PlatformUtils")

    def test_a_backslash_continued_conditional_cannot_evade_the_gate(self):
        # Regression: the regex was single-line, so the first line carried the #if with no
        # platform token and the second carried the token with no #.
        self._core("Private/e.cpp", "#if defined(FEATURE) || \\\n    defined(_WIN32)\nint x = 1;\n#endif\n")
        self.assertGateFails(self._gate(), "_WIN32")

    def test_compiler_macros_are_deliberately_allowed(self):
        # _MSC_VER is a compiler macro, not a platform macro. Assert.h branches on it, and
        # must be allowed to -- ADR-0012's rule is about platform portability.
        self._core("Private/a.cpp", "#if defined(_MSC_VER)\nint m = 1;\n#endif\n")
        self.assertGatePasses(self._gate())

    def test_apple_and_linux_macros_are_caught_too(self):
        self._core("Private/p.cpp", "#if defined(__APPLE__)\nint a = 1;\n#endif\n")
        self.assertGateFails(self._gate(), "__APPLE__")

    def test_a_neutral_file_beside_the_platform_directories_is_not_exempt(self):
        # Regression: the exemption once covered the whole Private/Platform/ tree, so a
        # Windows conditional could sit unnoticed in Path.cpp -- a file ADR-0016 makes
        # platform-neutral precisely so every platform shares it.
        self._core("Private/Platform/Path.cpp", WIN32_IFDEF)
        self.assertGateFails(self._gate(), "Platform/Path.cpp")

    def test_a_per_platform_directory_is_exempt(self):
        self._core("Private/Platform/Windows/File.cpp", WIN32_IFDEF)
        self.assertGatePasses(self._gate())

    def test_a_public_platform_header_is_not_exempt(self):
        # Public headers stay platform-neutral under ADR-0016, carrying opaque fixed-size
        # members rather than conditional ones.
        self._core("Include/Monarc/Core/Platform/Thread.h", WIN32_IFDEF)
        self.tree.mkdir("Source/Monarc.Core/Private")
        self.assertGateFails(self._gate(), "Thread.h")

    def test_a_platform_macro_in_a_test_is_ignored(self):
        # Tests are not modules; nothing may depend on them, and they are entitled to probe
        # platform behaviour. Only Include/ and Private/ are a module's own code.
        self._core("Tests/t.cpp", "#ifdef _WIN32\nint w = 1;\n#endif\n")
        self.tree.mkdir("Source/Monarc.Core/Private")
        self.assertGatePasses(self._gate())


class TestLogicalLines(unittest.TestCase):
    def test_plain_lines_keep_their_numbers(self):
        got = list(ca.logical_lines("alpha\nbeta\ngamma\n"))
        self.assertEqual(got, [(1, "alpha"), (2, "beta"), (3, "gamma")])

    def test_a_continuation_is_joined_and_reports_the_first_line(self):
        got = list(ca.logical_lines("one \\\ntwo\nthree\n"))
        self.assertEqual(got[0], (1, "one two"))
        self.assertEqual(got[1], (3, "three"))

    def test_several_continuations_join_into_one(self):
        got = list(ca.logical_lines("a \\\nb \\\nc\n"))
        self.assertEqual(got[0], (1, "a b c"))

    def test_a_trailing_continuation_at_end_of_file_still_yields(self):
        got = list(ca.logical_lines("dangling \\\n"))
        self.assertEqual(len(got), 1)


class TestIterSources(FixtureTest):
    def test_only_include_and_private_are_a_modules_own_code(self):
        self.tree.write("M/Include/h.h", "\n")
        self.tree.write("M/Private/c.cpp", "\n")
        self.tree.write("M/Tests/t.cpp", "\n")
        self.tree.write("M/stray.cpp", "\n")
        found = {p.name for p in ca.iter_sources(self.tree.root, "M")}
        self.assertEqual(found, {"h.h", "c.cpp"})

    def test_a_missing_module_directory_yields_nothing(self):
        self.assertEqual(list(ca.iter_sources(self.tree.root, "Absent")), [])

    def test_non_source_suffixes_are_skipped(self):
        self.tree.write("M/Private/notes.txt", "\n")
        self.tree.write("M/Private/.gitkeep", "")
        self.tree.mkdir("M/Include")
        self.assertEqual(list(ca.iter_sources(self.tree.root, "M")), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
