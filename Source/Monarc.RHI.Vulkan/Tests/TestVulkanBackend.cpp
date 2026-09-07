// The one translation unit in Monarc.RHI.Vulkan.Tests that provides doctest's main().
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <Monarc/Core/Error.h>
#include <Monarc/RHI/Vulkan/VulkanBackend.h>

// Task 1's contract, and only Task 1's. Task 2 replaces this with the loader and instance
// tests; it is worth writing now because a stub that quietly started reporting success -- or
// stopped saying why it failed -- would otherwise be indistinguishable from a working
// backend to everything except a human reading the log.
TEST_CASE("creating the Vulkan backend reports Unsupported, and says why") {
    const Monarc::Status backend = Monarc::RHI::CreateVulkanBackend();

    REQUIRE_FALSE(backend.has_value());
    CHECK(backend.error().code == Monarc::ErrorCode::Unsupported);

    // A caller that cannot tell a player what went wrong has no better information than a
    // crash. The message is part of the contract, not decoration.
    CHECK_FALSE(backend.error().message.empty());
}
