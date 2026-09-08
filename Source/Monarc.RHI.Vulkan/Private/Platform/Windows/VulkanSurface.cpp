// `vkCreateWin32SurfaceKHR`, and the only `<windows.h>` in Monarc.RHI.Vulkan.
//
// **The file the A3 plan names, added rather than folded into VulkanPlatform.cpp -- and that
// choice is recorded here because the plan's file listing is the thing being departed from
// in one direction and obeyed in the other.** Task 2 created
// Private/Platform/Windows/VulkanPlatform.cpp, which answers two questions that need no
// Vulkan object and no Windows header at all: what the loader library is called, and what the
// surface extension is called. Surface creation needs a `VkInstance`, an entry-point lookup,
// an `HWND` and `<windows.h>`. Keeping them apart is what lets the claim "exactly one file in
// this module includes `<windows.h>`" name a file whose entire job is the surface -- so the
// claim is checkable by looking at one file rather than by reading two.
//
// The checkable form, and it has to match the *include* rather than the word: one other file in
// this module mentions `<Windows.h>` in a comment -- Private/VulkanSwapchain.cpp, at the top,
// where it says surface creation is elsewhere -- so a grep for the bare name answers partly
// about prose. This one does not --
//
// (It said "two other files" until a review counted them:
// `grep -rniIl "windows\.h" Source/Monarc.RHI.Vulkan/` returns this file and that one. The
// companion claim in Monarc.Host.Windowed/Private/Platform/Windows/Window.cpp -- "three other
// files in this module" -- is exact: Window.h, WindowPlatform.h and Tests/TestWindow.cpp.)
//
//     grep -rniE '^[[:space:]]*#[[:space:]]*include[[:space:]]*<windows\.h>' Source/
//
// -- and it matches exactly one line in Monarc.RHI.Vulkan, this file's. (Case-insensitive
// because Win32 sources in this tree spell it `<Windows.h>`; a case-sensitive grep would be
// answering about capitalisation.)
//
// Under ADR-0016 the *directory* is what selects the file: monarc_module() hands the compiler
// only the current platform's Private/Platform/<Platform>/ tree, so a Linux build never sees
// this one rather than compiling it out of a conditional.
//
// **One thing the plan's checkbox puts here is deliberately elsewhere: the queue-family
// presentation-support query.** `vkGetPhysicalDeviceSurfaceSupportKHR` belongs to
// `VK_KHR_surface`, not to any platform's extension, so it needs neither `<Windows.h>` nor
// `VK_USE_PLATFORM_WIN32_KHR` -- putting it in this directory would put platform-*neutral* code
// in a per-platform tree, which is the mistake ADR-0016's own exemption note warns about from
// the other side. It lives in the two places that ask the question:
// `VulkanBackend::AdapterCanPresent`, which a caller asks before choosing a device, and
// `VulkanSwapchainFactory::Create`, which asks again because a caller is not obliged to have.
//
// **The define comes before every include, and that is load-bearing.**
// `VK_USE_PLATFORM_WIN32_KHR` is what makes `vulkan.h` pull in `vulkan_win32.h`, and
// `vulkan.h` has an include guard: if any header reached it first, the macro would be defined
// too late and `vkCreateWin32SurfaceKHR` would simply not exist -- a compile error whose cause
// is nowhere near its message. Private/VulkanSurface.h says why the declarations this file
// implements live in a header of their own rather than in VulkanPlatform.h, and this
// constraint is the reason.
#define VK_USE_PLATFORM_WIN32_KHR

// WIN32_LEAN_AND_MEAN and NOMINMAX before <windows.h>, matching every other Win32 translation
// unit in the tree (Monarc.Core/Private/Platform/Windows/*.cpp): the first drops the winsock,
// OLE and RPC headers nothing here uses, and the second stops the `min`/`max` function-like
// macros from breaking any header that has a member of either name.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <VulkanSurface.h>

#include <Monarc/Core/Log.h>

#include <Translate.h>
#include <VulkanPlatform.h>

namespace Monarc::RHI::Detail {

namespace {

MONARC_LOG_CATEGORY(VulkanSurface, Info);

}  // namespace

Status CreatePlatformSurface(PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance,
                             const SurfaceDescription& description, VkSurfaceKHR& out) {
    out = VK_NULL_HANDLE;

    if (getInstanceProcAddr == nullptr || instance == VK_NULL_HANDLE) {
        return Err(ErrorCode::InvalidArgument,
                   "a Vulkan surface cannot be created without an instance and its resolver");
    }
    if (description.nativeWindow == nullptr) {
        // `SurfaceDescription::IsEmpty()` tests this one handle for every platform; the
        // message names it, because on Windows it is the `HWND` and a caller who passed the
        // two handles in the wrong order gets told which one was empty.
        return Err(ErrorCode::InvalidArgument,
                   "a Vulkan surface cannot be created without a native window handle");
    }
    if (description.nativeDisplay == nullptr) {
        // Win32 needs both: `VkWin32SurfaceCreateInfoKHR` has an `hinstance` field and the
        // spec requires it to be the module the window belongs to. This is the check
        // `SurfaceDescription::IsEmpty()` deliberately does not make, because a platform whose
        // surface extension needs one handle would refuse a legitimate description.
        return Err(ErrorCode::InvalidArgument,
                   "a Win32 Vulkan surface needs the module handle its window belongs to as "
                   "well as the window itself");
    }

    // Resolved here rather than from a table, for the reason Private/VulkanSurface.h gives:
    // PFN_vkCreateWin32SurfaceKHR is a Win32-conditional type and Loader.h may not name one.
    // The cast is the one vkGetInstanceProcAddr's contract requires -- it returns a
    // PFN_vkVoidFunction and the caller supplies the type.
    const auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        getInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    if (create == nullptr) {
        // NotFound and not Unsupported: the extension *was* required at instance creation
        // (BringUp refuses an implementation that does not offer it), so a null here means the
        // loader enabled an extension and did not export its entry point. That is a broken
        // implementation rather than an absent capability, and NotFound is the code
        // Loader::Open already uses for a missing entry point.
        MONARC_LOG(VulkanSurface, Error,
                   "the instance was created with {} and vkGetInstanceProcAddr returned "
                   "nothing for vkCreateWin32SurfaceKHR",
                   PlatformSurfaceExtensionName());
        return Err(ErrorCode::NotFound,
                   "this Vulkan implementation does not export vkCreateWin32SurfaceKHR");
    }

    VkWin32SurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hinstance = static_cast<HINSTANCE>(description.nativeDisplay);
    surfaceInfo.hwnd      = static_cast<HWND>(description.nativeWindow);

    if (const VkResult result = create(instance, &surfaceInfo, nullptr, &out);
        result != VK_SUCCESS) {
        out = VK_NULL_HANDLE;
        MONARC_LOG(VulkanSurface, Warning, "vkCreateWin32SurfaceKHR failed: {} ({})",
                   ToString(result), static_cast<i32>(result));
        return Err(ToErrorCode(result), ToString(result));
    }
    return {};
}

}  // namespace Monarc::RHI::Detail
