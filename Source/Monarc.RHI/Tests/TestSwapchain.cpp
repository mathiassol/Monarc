// Device-free tests for the swapchain's *value* types: the surface description, the acquire
// outcome, and what an `AcquiredImage` says before anything has acquired one.
//
// **`ISwapchain` itself is not testable here and nothing pretends otherwise.** Every one of its
// members needs a window, a surface, a device and a presenting queue family, so the interface's
// behaviour is covered by `Monarc.Host.Windowed`'s device suite. What is left is small and worth
// having: two predicates and a default, each of which something in the backend or the frame loop
// branches on.

// Tests/TestTypes.cpp is the translation unit in this suite that provides doctest's main().
#include <doctest/doctest.h>

#include <Monarc/RHI/Swapchain.h>
#include <Monarc/RHI/Types.h>

#include <string_view>

using Monarc::RHI::AcquiredImage;
using Monarc::RHI::AcquireOutcome;
using Monarc::RHI::Format;
using Monarc::RHI::SurfaceDescription;
using Monarc::RHI::SwapchainDescription;

TEST_CASE("a surface description is empty until it names a window") {
    // `IsEmpty()` tests the *window* handle alone, and the asymmetry is deliberate: it is the
    // handle every platform's surface extension requires, where a display or module handle is
    // needed by some and not others. A platform that needs both refuses a missing one itself,
    // naming which -- `Detail::CreatePlatformSurface` in Monarc.RHI.Vulkan does exactly that.
    CHECK(SurfaceDescription{}.IsEmpty());

    // The half that would go red if `IsEmpty()` tested both handles: a description with a
    // window and no display is *not* empty, and refusing it is the platform's job.
    int                     window  = 0;
    SurfaceDescription      onlyWindow{};
    onlyWindow.nativeWindow = &window;
    CHECK_FALSE(onlyWindow.IsEmpty());

    // And the half that would go red if it tested the display handle instead.
    int                sink = 0;
    SurfaceDescription onlyDisplay{};
    onlyDisplay.nativeDisplay = &sink;
    CHECK(onlyDisplay.IsEmpty());
}

TEST_CASE("a swapchain description defaults to what Phase A3 commits to") {
    const SwapchainDescription description;

    // B8G8R8A8_UNORM is the format the phase plan names, and the readback assertions in
    // Monarc.Host.Windowed's device suite are statements about it. A default that had drifted
    // would make those assertions about something else.
    CHECK(description.format == Format::B8G8R8A8_UNORM);

    // The two that must be supplied: there is no sensible default surface and no sensible
    // default size, and both are refused by `CreateSwapchain` rather than guessed at.
    CHECK(description.surface.IsEmpty());
    CHECK(description.extent.IsEmpty());

    // Readback is opt-in. A default of true would ask every swapchain in every shipped program
    // for a usage bit only a test needs.
    CHECK_FALSE(description.allowReadback);
}

TEST_CASE("an acquired image defaults to having acquired nothing") {
    const AcquiredImage image;

    // **`OutOfDate` and not `Acquired`, which is the conservative default rather than the tidy
    // one.** A caller who forgets to read `outcome` gets "there is no image"; the other default
    // would have them render into an invalid handle and present it.
    CHECK(image.outcome == AcquireOutcome::OutOfDate);
    CHECK_FALSE(image.texture.IsValid());
    CHECK(image.index == 0);
    CHECK_FALSE(image.suboptimal);
}

TEST_CASE("every AcquireOutcome has its own spelling") {
    CHECK(std::string_view(Monarc::RHI::ToString(AcquireOutcome::Acquired)) == "Acquired");
    CHECK(std::string_view(Monarc::RHI::ToString(AcquireOutcome::OutOfDate)) == "OutOfDate");

    // A value outside the enumerator set gets a name of its own rather than one of the two
    // above -- `ToString(Format)`'s rule in Types.h, and for its reason: a log that reported an
    // invalid outcome as `OutOfDate` would send its reader after a swapchain recreation that
    // was never asked for.
    const std::string_view invalid =
        Monarc::RHI::ToString(static_cast<AcquireOutcome>(4242));
    CHECK(invalid != "Acquired");
    CHECK(invalid != "OutOfDate");
}
