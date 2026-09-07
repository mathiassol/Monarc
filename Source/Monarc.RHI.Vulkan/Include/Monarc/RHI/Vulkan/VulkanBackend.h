#pragma once

#include <Monarc/Core/Error.h>

// namespace Monarc::RHI rather than Monarc::RHI::Vulkan, deliberately. What this module
// exports is an RHI backend; its Vulkan-ness belongs in the name of the thing, not in the
// namespace of its only entry point -- Monarc::RHI::CreateVulkanBackend() reads as what it
// is, where Monarc::RHI::Vulkan::CreateVulkanBackend() says Vulkan twice and still does not
// say backend once.
namespace Monarc::RHI {

/// Brings up the Vulkan backend: opens the loader, creates an instance, and hands back
/// something that can enumerate adapters and create a device.
///
/// **Phase A3 Task 1 has none of that yet**, so this reports ErrorCode::Unsupported with a
/// message saying so, and Task 2 replaces it. The signature will grow a return value then --
/// there is no IBackend to return today, and declaring one before there is anything behind
/// it would be a promise the linker keeps and the code does not.
///
/// A missing Vulkan runtime is an ordinary error here rather than a crash before main. That
/// is the reason Monarc opens vulkan-1.dll itself through Platform::Library instead of
/// linking the import library: see the loader decision in the Phase A3 plan.
[[nodiscard]] Status CreateVulkanBackend();

}  // namespace Monarc::RHI
