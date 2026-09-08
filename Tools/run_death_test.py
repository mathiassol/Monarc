#!/usr/bin/env python3
"""Runs one of Monarc's fatal guards in a child process and asserts that it fired.

Monarc has guards that deliberately end the process rather than continue with state that is
already wrong -- `Array<T>::OnAllocationFailed`, `JobSystem::Wait`'s worker guard,
`ICommandList::Barrier`'s stale-handle refusal, and others. Each was verified once by hand with
a scratch program that was then deleted, so nothing stopped a later edit from turning any of
them back into a silent `return`. They cannot be tested in process: the assertion is that the
process *dies*.

Usage:  run_death_test.py <test-executable> <guard-name> <expected-message> [child args...]

Runs `<test-executable> --monarc-death-guard=<guard-name>`, which invokes exactly that guard
with an assert handler installed that declines to break -- see any of the
`Tests/TestDeathGuards.cpp` files for why that is the interesting configuration and not a
contrivance. Exit zero when the guard fired, non-zero when it did not, and **77 when the child
reported 77**, so a device-required guard on a machine with no GPU reports Skipped through the
same SKIP_RETURN_CODE the rest of the suite uses.

**Four conditions, and each one is load-bearing.**

  1. The child printed `MONARC_DEATH_GUARD_ENTERED <name>`. Without this a mistyped guard name
     -- or a binary that rejected the option -- would exit non-zero having run nothing, and
     that is indistinguishable from a guard firing. This is what makes the test unable to pass
     by doing nothing.
  2. The child did **not** print `MONARC_DEATH_GUARD_SURVIVED <name>`, which is the line
     printed immediately after the guard call returns. This is the condition a regression
     turns red: a guard edited into a plain `return` reaches that line.
  3. The exit code is non-zero. Deliberately *not* an exact value. On Windows the code is the
     **debug break's** (`0x80000003`, which Python reports as -2147483645) and not `abort`'s,
     because `MONARC_DEBUG_BREAK()` runs first; under doctest's SEH handler the same break can
     surface as 1 instead, which is why the guards are invoked from `main` before doctest is
     started. A test asserting one number would be asserting about the harness.
  4. The expected message appears in the output. That message is the guard's own `MONARC_CHECK`
     literal, so this is what says the *right* guard fired rather than some other failure on
     the way to it.

A child that neither dies nor returns is a **failure**, not a death: `JobSystem::Wait`'s guard
with its abort removed falls into a condition-variable wait and hangs forever, and reporting a
hang as a caught guard would make that exact regression invisible. Hence the timeout below,
which is deliberately shorter than the CTest TIMEOUT on these entries so that this message is
the one a reader sees.

Trailing arguments are passed through to the child. **Nothing CMake registers uses them, and
they exist to make the skip path provable**: `--vulkan-library=not-vulkan` makes the device
suite's `main` fail exactly as it would on a machine with no Vulkan, so the 77 propagation above
can be observed on a machine that has one. That is the same lever `TestsDevice/` already
carries for its own suite.

Standard library only, and no third-party test runner: this is registered directly with CTest,
one entry per guard. Three Python interpreters exist on the development machine and CI uses a
fourth, so which one runs this depends on how it is invoked -- CMake passes
`${Python3_EXECUTABLE}`.
"""
from __future__ import annotations

import os
import subprocess
import sys

#: Printed by the child immediately before and immediately after the guard call.
#:
#: Spelled here and in each Tests/TestDeathGuards.cpp. A mismatch between the two makes every
#: death test fail loudly -- condition 1 above stops being satisfiable -- rather than pass
#: silently, which is the only direction a duplicated constant is allowed to break in.
ENTERED_MARKER = "MONARC_DEATH_GUARD_ENTERED"
SURVIVED_MARKER = "MONARC_DEATH_GUARD_SURVIVED"

#: CTest's SKIP_RETURN_CODE, as used by every device-required entry in this tree.
SKIP_RETURN_CODE = 77

#: Long enough that a machine bringing up Vulkan is never rushed (the whole device suite's
#: slowest run is 8.79 s and one guard does a fraction of that), short enough that a guard
#: edited into a hang is reported in half a minute rather than by CTest's own timeout.
CHILD_TIMEOUT_SECONDS = 30


def main(argv: list[str]) -> int:
    if len(argv) < 4:
        print(__doc__)
        return 2

    executable, guard, expected = argv[1], argv[2], argv[3]
    extra = argv[4:]

    # Normalised because one of this machine's Python interpreters -- the MSYS2 one -- has a
    # `CreateProcess` that refuses a *relative* path written with forward slashes, and reports
    # it as "the system cannot find the file specified" for a file that is plainly there. CMake
    # passes an absolute native path so CTest never meets this; a developer running the harness
    # by hand from the repository root does, and the diagnostic sends them looking in the wrong
    # place.
    executable = os.path.normpath(os.path.abspath(executable))
    command = [executable, f"--monarc-death-guard={guard}", *extra]

    try:
        # stderr merged into stdout: the declining assert handler writes to stderr and the
        # markers to stdout, and the conditions below are about the whole of what the child
        # said rather than about which stream it chose.
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=CHILD_TIMEOUT_SECONDS,
            text=True,
            errors="replace",
        )
    except FileNotFoundError:
        print(f"death test {guard}: cannot run {executable}")
        return 1
    except subprocess.TimeoutExpired as expired:
        output = expired.output or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        print(f"death test {guard}: FAILED -- the child neither died nor returned within "
              f"{CHILD_TIMEOUT_SECONDS} s.")
        print("A guard that hangs instead of stopping is the regression this exists to catch: "
              "JobSystem::Wait's guard with its abort removed waits on a condition variable "
              "forever.")
        _dump(command, output)
        return 1

    output = completed.stdout or ""

    if completed.returncode == SKIP_RETURN_CODE:
        # The child's own main decided it could not run -- no Vulkan runtime, no adapter, no
        # interactive session. Propagated rather than swallowed, so CTest reports Skipped and
        # never Passed. Note the ordering: this is checked before the conditions below, because
        # a skipping child never reaches the guard and must not be judged as though it had.
        print(f"death test {guard}: SKIPPED -- the child returned {SKIP_RETURN_CODE}.")
        _dump(command, output)
        return SKIP_RETURN_CODE

    failures: list[str] = []
    entered = f"{ENTERED_MARKER} {guard}"
    survived = f"{SURVIVED_MARKER} {guard}"

    if entered not in output:
        failures.append(f"the child never printed \"{entered}\", so it did not reach the "
                        f"guard at all -- an unknown guard name, or a binary that rejected "
                        f"--monarc-death-guard")
    if survived in output:
        failures.append(f"the child printed \"{survived}\": the guard returned instead of "
                        f"ending the process")
    if completed.returncode == 0:
        failures.append("the child exited zero")
    if expected not in output:
        failures.append(f"the expected message was not in the output: \"{expected}\"")

    if failures:
        print(f"death test {guard}: FAILED")
        for failure in failures:
            print(f"  - {failure}")
        _dump(command, output, completed.returncode)
        return 1

    print(f"death test {guard}: the guard ended the process with exit code "
          f"{completed.returncode} ({_hex(completed.returncode)}) and said "
          f"\"{expected}\"")
    return 0


def _hex(code: int) -> str:
    """The exit code as Windows writes it: 0x80000003 rather than -2147483645."""
    return f"0x{code & 0xFFFFFFFF:08X}"


def _dump(command: list[str], output: str, code: int | None = None) -> None:
    print(f"  command: {' '.join(command)}")
    if code is not None:
        print(f"  exit code: {code} ({_hex(code)})")
    print("  ---- child output ----")
    for line in output.splitlines():
        print(f"  {line}")
    print("  ----------------------")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
