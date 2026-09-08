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


def module(name, tier=0, kind="Runtime", directory="M", public=None, private=None, app=None):
    entry = {
        "name": name,
        "kind": kind,
        "tier": tier,
        "directory": directory,
        "publicDeps": public or [],
        "privateDeps": private or [],
    }
    # Omitted rather than defaulted to False, deliberately: most fixtures then have the
    # shape of a module-graph.json written before "app" existed, so every gate that reads
    # the key is exercised against a graph that does not carry it.
    if app is not None:
        entry["app"] = app
    return entry


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


class TestAppLeaves(FixtureTest):
    def test_a_graph_with_no_apps_passes(self):
        self.assertGatePasses(ca.gate_app_leaves(graph(module("A", public=["B"]), module("B"))))

    def test_an_app_depending_on_modules_is_accepted(self):
        # An app is a leaf, not an island: linking the graph together is its entire job,
        # and Monarc.FirstLight exists precisely to depend on four modules at once.
        g = graph(
            module("Monarc.FirstLight", tier=3, directory="F", app=True,
                   public=["Monarc.RHI", "Monarc.Host.Windowed"]),
            module("Monarc.RHI", tier=2, directory="R"),
            module("Monarc.Host.Windowed", tier=3, directory="H"),
        )
        self.assertGatePasses(ca.gate_app_leaves(g))

    def test_a_module_publicly_depending_on_an_app_is_rejected(self):
        g = graph(
            module("Monarc.RHI", tier=2, directory="R", public=["Monarc.FirstLight"]),
            module("Monarc.FirstLight", tier=3, directory="F", app=True),
        )
        self.assertGateFails(ca.gate_app_leaves(g), "Monarc.FirstLight")

    def test_a_module_privately_depending_on_an_app_is_rejected(self):
        # Both dependency kinds are edges. Checking only publicDeps would miss this.
        g = graph(
            module("Monarc.RHI", tier=2, directory="R", private=["Monarc.FirstLight"]),
            module("Monarc.FirstLight", tier=3, directory="F", app=True),
        )
        self.assertGateFails(ca.gate_app_leaves(g), "Monarc.FirstLight")

    def test_one_app_depending_on_another_app_is_rejected(self):
        # Being an app is not a licence to link one: it is still a second main().
        g = graph(module("A", app=True, public=["B"]), module("B", app=True))
        self.assertGateFails(ca.gate_app_leaves(g), "B")

    def test_a_dependency_on_an_undeclared_module_is_ignored_here(self):
        # CMake rejects unknown dependencies at configure time; this gate must not crash
        # on an edge pointing outside the graph. Same division of labour as gate 2.
        self.assertGatePasses(ca.gate_app_leaves(graph(module("X", public=["Absent"]))))

    def test_a_graph_written_before_the_app_key_existed_passes(self):
        g = graph(module("A", public=["B"]), module("B"))
        self.assertNotIn("app", g["B"])
        self.assertGatePasses(ca.gate_app_leaves(g))


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

    def test_tier2_including_a_tier3_host_header_fails(self):
        # The first entry in the forbidden list naming a module that exists. Monarc.RHI
        # and Monarc.RHI.Vulkan are Tier 2 and Monarc.Host.Windowed is Tier 3, so this is
        # the direction gate 3 can finally be made to fail in on purpose.
        self.assertGateFails(
            self._tier2_with_include('#include <Monarc/Host/Window.h>'), "Monarc/Host/"
        )

    def test_tier2_including_core_passes(self):
        self.assertGatePasses(self._tier2_with_include('#include <Monarc/Core/Types.h>'))

    def test_tier2_including_another_tier2_header_passes(self):
        # Monarc.RHI.Vulkan includes Monarc/RHI/ headers by design: ADR-0007 puts Tier 2
        # modules inside one package, and it is Tier 1 and Tier 3 that are out of bounds.
        self.assertGatePasses(self._tier2_with_include('#include <Monarc/RHI/Types.h>'))

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


def test_target(name, owner, links):
    return {"name": name, "module": owner, "links": links}


class TestTestLinks(FixtureTest):
    """Gate 14: a test target links only what its own module may depend on.

    Every case here builds the real tree's shape, because the rule exists for one real
    situation: Monarc.Host.Windowed.DeviceTests links Monarc.RHI.Vulkan, and that has to stay
    legal while the reverse direction becomes an error.
    """

    #: Tiers as A3 declares them, so a case reads as the tree rather than as a fixture.
    REAL = graph(
        module("Monarc.Core", tier=0, directory="Source/Monarc.Core"),
        module("Monarc.Assets", tier=1, directory="Source/Monarc.Assets"),
        module("Monarc.RHI", tier=2, directory="Source/Monarc.RHI"),
        module("Monarc.RHI.Vulkan", tier=2, directory="Source/Monarc.RHI.Vulkan"),
        module("Monarc.Host.Windowed", tier=3, directory="Source/Monarc.Host.Windowed"),
        module("Monarc.FirstLight", tier=3, directory="Source/Monarc.FirstLight", app=True),
    )

    def test_no_test_targets_passes(self):
        # -DMONARC_BUILD_TESTS=OFF emits no testTargets, and a graph written before the key
        # existed carries none either.
        self.assertGatePasses(ca.gate_test_links(self.REAL, []))

    def test_a_tier3_test_linking_a_tier2_module_is_accepted(self):
        # The tree as it actually is: Monarc.Host.Windowed.DeviceTests links Monarc.RHI.Vulkan
        # so that a swapchain can be built against a real window. Tier 3 reaching down to
        # tier 2 is the direction the graph already allows dependencies to run, and a rule
        # that broke this would be a rule nobody could keep.
        self.assertGatePasses(ca.gate_test_links(self.REAL, [
            test_target("Monarc.Host.Windowed.DeviceTests", "Monarc.Host.Windowed",
                        ["Monarc.Host.Windowed", "doctest::doctest", "Monarc.RHI.Vulkan"]),
        ]))

    def test_a_tier2_test_linking_a_tier3_module_is_rejected(self):
        # The case gate 3 cannot see. Putting the swapchain cases in
        # Monarc.RHI.Vulkan/TestsDevice/ instead would need this link line, and the include of
        # Monarc/Host/Window.h that follows it is invisible to every source-reading gate.
        self.assertGateFails(ca.gate_test_links(self.REAL, [
            test_target("Monarc.RHI.Vulkan.DeviceTests", "Monarc.RHI.Vulkan",
                        ["Monarc.RHI.Vulkan", "Monarc.Host.Windowed"]),
        ]), "Monarc.Host.Windowed")

    def test_a_tier2_test_linking_a_tier1_module_is_rejected(self):
        # Downward by tier alone would permit this, and it must not: ADR-0007's package
        # boundary is what gate 3 implements, and tier 1 is on the wrong side of it just as
        # tier 3 is. A link rule that only refused *upward* edges would be a different rule
        # from the one it is meant to complete.
        self.assertGateFails(ca.gate_test_links(self.REAL, [
            test_target("Monarc.RHI.Tests", "Monarc.RHI",
                        ["Monarc.RHI", "Monarc.Assets"]),
        ]), "renderer package boundary")

    def test_a_tier2_test_linking_another_tier2_module_is_accepted(self):
        # Inside the package. Nothing does this today; Monarc.RHI's tests linking
        # Monarc.RHI.Vulkan to check the abstraction against a backend is the obvious future
        # caller, and the rule has to let it through.
        self.assertGatePasses(ca.gate_test_links(self.REAL, [
            test_target("Monarc.RHI.Tests", "Monarc.RHI",
                        ["Monarc.RHI", "Monarc.RHI.Vulkan"]),
        ]))

    def test_a_tier0_test_linking_a_tier0_module_is_accepted(self):
        self.assertGatePasses(ca.gate_test_links(self.REAL, [
            test_target("Monarc.Core.Tests", "Monarc.Core", ["Monarc.Core"]),
        ]))

    def test_a_test_linking_an_app_is_rejected(self):
        # A second main(), and the linker would say so -- but rule 8 says it first, and a
        # test target is exactly the place someone would try it to get at an app's internals.
        self.assertGateFails(ca.gate_test_links(self.REAL, [
            test_target("Monarc.Host.Windowed.Tests", "Monarc.Host.Windowed",
                        ["Monarc.Host.Windowed", "Monarc.FirstLight"]),
        ]), "apps are graph leaves")

    def test_non_module_link_entries_are_ignored(self):
        # doctest and Vulkan::Headers have no tier. They are emitted into module-graph.json
        # because that key records what the target links, and skipped here because this gate
        # is about the module graph -- third-party dependencies are ADR-0014's business.
        self.assertGatePasses(ca.gate_test_links(self.REAL, [
            test_target("Monarc.RHI.Vulkan.Tests", "Monarc.RHI.Vulkan",
                        ["Monarc.RHI.Vulkan", "doctest::doctest", "Vulkan::Headers"]),
        ]))

    def test_a_test_whose_module_is_absent_from_the_graph_is_ignored(self):
        # Cannot happen -- _monarc_add_test_binary is called by the module's own
        # CMakeLists.txt -- and must not crash if it ever does. Gates 2 and 12 treat an edge
        # pointing outside the graph the same way.
        self.assertGatePasses(ca.gate_test_links(self.REAL, [
            test_target("Ghost.Tests", "Ghost", ["Monarc.Host.Windowed"]),
        ]))

    def test_the_two_forbidden_tables_police_the_same_tiers(self):
        # FORBIDDEN_INCLUDES names header prefixes and FORBIDDEN_LINK_TIERS names tiers, so
        # they cannot be one constant. This catches the drift that matters: a boundary added
        # to one table and forgotten in the other, which would leave gate 3 and gate 14
        # disagreeing about which tier is inside the package.
        self.assertEqual(sorted(ca.FORBIDDEN_INCLUDES), sorted(ca.FORBIDDEN_LINK_TIERS))


class TestLayout(FixtureTest):
    def test_both_directories_present_passes(self):
        self.tree.mkdir("M/Include")
        self.tree.mkdir("M/Private")
        self.assertGatePasses(ca.gate_layout(graph(module("M", directory="M")), self.tree.root))

    def test_missing_include_fails(self):
        # The fixture carries no "app" key at all, so this also covers the absent-key path
        # through the exemption below.
        self.tree.mkdir("M/Private")
        self.assertGateFails(
            ca.gate_layout(graph(module("M", directory="M")), self.tree.root), "Include"
        )

    def test_a_module_explicitly_marked_not_an_app_still_needs_include(self):
        self.tree.mkdir("M/Private")
        self.assertGateFails(
            ca.gate_layout(graph(module("M", directory="M", app=False)), self.tree.root),
            "Include",
        )

    def test_an_app_needs_no_include_directory(self):
        # Nothing may depend on an app, so it has no public interface to expose. Requiring
        # Include/ of it would mean an empty directory existing purely to satisfy a gate.
        self.tree.mkdir("A/Private")
        self.assertGatePasses(
            ca.gate_layout(graph(module("A", directory="A", app=True)), self.tree.root)
        )

    def test_an_app_without_private_still_fails(self):
        # Include/ is the only relaxation. An app with no Private/ has no source at all,
        # which is a broken module however few consumers it has.
        self.tree.mkdir("A")
        self.assertGateFails(
            ca.gate_layout(graph(module("A", directory="A", app=True)), self.tree.root),
            "Private",
        )

    def test_an_app_that_has_an_include_directory_fails(self):
        # Not just unnecessary -- wrong, and quietly so. monarc_app() does not glob an app's
        # Include/, so nothing in the build ever reads a header left there while the gates
        # still do -- iter_sources walks Include/ for every module, app or not. Saying an app
        # must not have one closes that gap; merely excusing it does not.
        self.tree.mkdir("A/Include")
        self.tree.mkdir("A/Private")
        self.assertGateFails(
            ca.gate_layout(graph(module("A", directory="A", app=True)), self.tree.root),
            "must not have",
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

    # -------------------------------------------------------------------------------------
    # The rule is path-shaped and module-agnostic, and until A3 Task 5 every case above built
    # a Monarc.Core-shaped tree -- so the gate's own name ("platform conditionals only in
    # Core/Platform") could have been made true of the mechanism without one of them
    # noticing. These two build a *non-Core* module and pin both directions.
    # -------------------------------------------------------------------------------------

    def _host_gate(self):
        return ca.gate_platform_containment(
            graph(module("Monarc.Host.Windowed", tier=3,
                         directory="Source/Monarc.Host.Windowed")),
            self.tree.root,
        )

    def _host(self, relative, text):
        self.tree.mkdir("Source/Monarc.Host.Windowed/Include")
        self.tree.write(f"Source/Monarc.Host.Windowed/{relative}", text)

    def test_a_platform_ifdef_in_a_non_core_module_fails(self):
        # A3 is what makes this reachable rather than hypothetical: Monarc.Host.Windowed and
        # Monarc.RHI.Vulkan both contain genuinely platform-specific code and neither is
        # Monarc.Core. Narrowing the gate to Monarc.Core -- which its old name invited --
        # turns this case red.
        self._host("Private/Window.cpp", WIN32_IFDEF)
        self.assertGateFails(self._host_gate(), "_WIN32")

    def test_a_per_platform_directory_in_a_non_core_module_is_exempt(self):
        # The other half of the same claim, and the half a Core-only exemption would break:
        # Private/Platform/<Platform>/ earns the exemption in any module, not only in the one
        # that happened to need it first. This is where the real
        # Private/Platform/Windows/Window.cpp lives.
        self._host("Private/Platform/Windows/Window.cpp", WIN32_IFDEF)
        self.assertGatePasses(self._host_gate())


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
