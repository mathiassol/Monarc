#pragma once

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Memory/Allocator.h>
#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Adapter.h>
#include <Monarc/RHI/Capabilities.h>

// namespace Monarc::RHI rather than Monarc::RHI::Vulkan, deliberately. What this module
// exports is an RHI backend; its Vulkan-ness belongs in the name of the thing, not in the
// namespace of its entry points -- Monarc::RHI::VulkanBackend reads as what it is, where
// Monarc::RHI::Vulkan::VulkanBackend says Vulkan twice and still does not say backend once.
namespace Monarc::RHI {

/// Whether a backend asks for the validation layer and the debug-utils messenger unless told
/// otherwise: true in Debug, false in every other configuration.
///
/// A function rather than a macro in this header, on purpose. The decision is made in this
/// module's own translation unit, from a definition this module's CMakeLists.txt sets
/// PRIVATE, so a consumer compiled with different flags cannot end up disagreeing with the
/// backend about whether validation is on -- which is exactly the class of bug that produces
/// a build believing it has validation while the instance never loaded the layer.
[[nodiscard]] bool ValidationDefault();

/// The Vulkan runtime, an instance, and the adapters that instance can see.
///
/// Owns three things: the `vulkan-1.dll` handle and its entry-point tables, the `VkInstance`,
/// and -- in Debug -- the `VK_EXT_debug_utils` messenger. Nothing here creates a logical
/// device, a queue or a command buffer; those are Task 3's, along with the `IDevice` this
/// class will end up sitting behind.
///
/// **This shape is a stated seam.** The A3 plan's architecture puts an `IBackend` interface in
/// `Monarc.RHI` with `Monarc.RHI.Vulkan` implementing it, and this is not that -- it is a
/// concrete move-only class in the backend's own public header. Two reasons, one of them
/// temporary: there is no `IDevice` yet for virtual dispatch to dispatch *to*, so an interface
/// now would be one implementation behind one vtable; and `Monarc.Core` has no owning-pointer
/// type, so returning an `IBackend` would mean inventing an ownership convention in Core to
/// serve a single caller. Task 3 introduces `IDevice`, and at that point this class becomes
/// the implementation behind an `IBackend` and `CreateVulkanBackend()` comes back as the
/// factory. Callers written against this class will need to change; saying so here is the
/// point of writing it down.
///
/// **Two-phase: construct, then `Initialize`.** Not the house factory shape
/// (`Platform::Library::Open` and `Host::Window::Create` both return a `Result<T>`), and the
/// reason is `Error::message`: it is a non-owning view over storage that must outlive the
/// error, and the messages this class produces name things that are not known until run time
/// -- the library that would not open, the `VkResult` a call returned, the extension that was
/// missing. That storage has to live somewhere, and the only somewhere that is neither a
/// hidden global nor a caller-supplied buffer is this object, which therefore has to exist
/// before the call that fails. A `Result<VulkanBackend>` would hand back a message pointing
/// into an object that was never returned.
///
/// A failure message consequently stays valid **only as long as this object does**, and only
/// until the next failed call on it. That is as long as a caller inspecting the `Status` it
/// just received needs, and it is why `Initialize` is called on a backend the caller holds.
///
/// Move-only and non-copyable, in the shape `Platform::Library` and `Host::Window` use: it
/// owns resources that must be released exactly once.
class VulkanBackend {
public:
    struct Config {
        /// Reported to the driver and to tools as `VkApplicationInfo::pApplicationName`. Not
        /// owned; must outlive the `Initialize` call. A string literal is the intended use,
        /// matching `Error::message` and `Host::WindowDescription::title`.
        const char* applicationName = "Monarc";

        /// Vulkan runtime library to open. `nullptr` -- the default -- means the platform's
        /// own, which is `vulkan-1.dll` on Windows and is chosen inside the module so no
        /// platform name appears in this header (ADR-0016).
        ///
        /// A name is settable for exactly one reason: it is how a test drives the
        /// library-not-found path, by naming something that cannot exist. No environment
        /// variable and no build-time switch is involved, and shipped code behaves the same
        /// whether or not a test ever sets it.
        const char* libraryName = nullptr;

        /// Whether to ask for `VK_LAYER_KHRONOS_validation` and `VK_EXT_debug_utils`.
        ///
        /// Asking is not getting. A machine with no Vulkan SDK has neither, and that is a
        /// warning and a degraded instance rather than a failure -- the game still runs.
        /// `ValidationLayerEnabled()` and `DebugMessengerInstalled()` report what actually
        /// happened, and a build that quietly failed to load the layer is therefore
        /// distinguishable from one that never asked.
        bool validation = ValidationDefault();
    };

    /// `allocator` is used for enumeration scratch space and for this object's own state, and
    /// must outlive the backend. Nothing here allocates outside it.
    ///
    /// This constructor performs the one allocation the class makes. If that allocation
    /// fails, the object is left unusable rather than aborting: `Initialize` then reports
    /// `ErrorCode::OutOfMemory` and `IsInitialized()` stays false.
    explicit VulkanBackend(IAllocator& allocator);

    /// Destroys the messenger, the instance, and the loader, in that order. See `Shutdown`.
    ~VulkanBackend();

    VulkanBackend(const VulkanBackend&)            = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

    VulkanBackend(VulkanBackend&& other) noexcept;
    VulkanBackend& operator=(VulkanBackend&& other) noexcept;

    /// Opens the loader, creates the instance, and installs the debug messenger if one was
    /// asked for and is available.
    ///
    /// On failure nothing is left open: a partially built backend is torn down before this
    /// returns, so `IsInitialized()` is false and calling `Initialize` again is safe.
    /// Failures are `ErrorCode::NotFound` (no Vulkan runtime, or a missing entry point),
    /// `ErrorCode::Unsupported` (the loader or a required extension is too old or absent) or
    /// `ErrorCode::BackendFailure` (a Vulkan call returned an error, named in the message).
    [[nodiscard]] Status Initialize(const Config& config);

    /// Tears everything down. Safe to call unconditionally and more than once.
    void Shutdown();

    [[nodiscard]] bool IsInitialized() const;

    /// The version the *loader* reports, from `vkEnumerateInstanceVersion` -- not any
    /// device's. `{0, 0, 0}` before a successful `Initialize`. On the development machine this
    /// is 1.4.357 while one of the two devices is 1.3.275, which is why the two numbers are
    /// kept apart: it is the device's that decides what may be called on it.
    [[nodiscard]] ApiVersion InstanceApiVersion() const;

    /// Whether `VK_LAYER_KHRONOS_validation` was actually enabled on the instance.
    [[nodiscard]] bool ValidationLayerEnabled() const;

    /// Whether a `VK_EXT_debug_utils` messenger is installed. When it is, a validation error
    /// stops the process at the message -- see the file comment in Private/VulkanBackend.cpp.
    [[nodiscard]] bool DebugMessengerInstalled() const;

    /// Every physical device the instance can see, in the order Vulkan reported them,
    /// **duplicates included**. `out` is cleared first: enumeration is a snapshot, not
    /// something to accumulate.
    ///
    /// On the development machine this returns five entries for two devices. That is not a
    /// defect in Vulkan or in this function -- see `DeduplicateAdapters` in
    /// Monarc/RHI/Adapter.h -- and having the raw list available is what makes the difference
    /// observable rather than a claim in a comment.
    [[nodiscard]] Status EnumerateAdaptersRaw(Array<AdapterInfo>& out);

    /// `EnumerateAdaptersRaw` followed by `DeduplicateAdapters`: every adapter the instance
    /// can see, once each, in the order Vulkan first reported it. This is the list a caller
    /// choosing a device should use.
    [[nodiscard]] Status EnumerateAdapters(Array<AdapterInfo>& out);

private:
    /// Shuts down and then destroys and deallocates the state. What the destructor and the
    /// move-assignment operator share, and the only private member of this class that names
    /// no Vulkan type -- everything else lives on State, whose definition is in
    /// Private/VulkanBackend.cpp because it names plenty of them.
    void Release();

    /// Everything this class owns lives behind one pointer, allocated from the caller's
    /// allocator, because the state names Vulkan types and a public header may not
    /// (Docs/Rendering/RHI.md, and ADR-0014's rule 1). The alternative -- opaque fixed-size
    /// storage, as `Platform::Library` uses for one `HMODULE` -- does not scale to three
    /// entry-point tables and a `Platform::Library`: the size would be a number in this
    /// header that a static_assert in the .cpp polices, and every table added later would
    /// change it.
    ///
    /// Null only if the constructor's allocation failed.
    struct State;
    State* m_state = nullptr;
};

}  // namespace Monarc::RHI
