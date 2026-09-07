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
/// factory. Callers written against `Create` will need to change; saying so here is the point
/// of writing it down.
///
/// **Factory construction**, as `Platform::Library::Open` and `Host::Window::Create` both do:
/// a static `Create` returning a `Result<VulkanBackend>`, a `Shutdown` safe to call
/// unconditionally, and an `IsInitialized` query. A backend a caller holds is therefore a
/// backend that came up -- there is no half-built one to hand back, and no allocation failure
/// to report after the fact, because the allocation happens inside `Create` before anything
/// is constructed.
///
/// `Error::message` is a non-owning view, so every failure this class reports is a string
/// literal and stays valid as long as the program does. The composed detail -- which library,
/// which `VkResult` and its numeric value, which version was reported -- goes to `MONARC_LOG`
/// at the failure site instead. Nothing branches on message text: the `ErrorCode` carries what
/// is branchable and the log carries what a human reads.
///
/// Move-only and non-copyable: it owns resources that must be released exactly once. There is
/// no default constructor -- unlike `Platform::Library`, whose storage is one inline handle, a
/// backend owns a heap allocation, so a default-constructed one would be a permanently dead
/// object rather than a cheap closed handle. A backend that has been moved from is that dead
/// object, and every query below answers on one rather than dereferencing.
class VulkanBackend {
public:
    struct Config {
        /// Reported to the driver and to tools as `VkApplicationInfo::pApplicationName`. Not
        /// owned; must outlive the `Create` call. A string literal is the intended use,
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

    /// Destroys the messenger, the instance, and the loader, in that order, and then releases
    /// the state. See `Shutdown`.
    ~VulkanBackend();

    VulkanBackend(const VulkanBackend&)            = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

    VulkanBackend(VulkanBackend&& other) noexcept;
    VulkanBackend& operator=(VulkanBackend&& other) noexcept;

    /// Opens the loader, creates the instance, and installs the debug messenger if one was
    /// asked for and is available. A returned backend is up.
    ///
    /// `allocator` is used for this object's own state and for enumeration scratch space, and
    /// must outlive the backend. Nothing here allocates outside it.
    ///
    /// Nothing survives a failure: the state is destroyed and deallocated before this
    /// returns, so a failed `Create` leaves the allocator exactly as it found it. Failures
    /// are `ErrorCode::OutOfMemory` (the allocator returned nothing), `ErrorCode::NotFound`
    /// (no Vulkan runtime, or a missing entry point), `ErrorCode::Unsupported` (the loader or
    /// a required extension is too old or absent) or `ErrorCode::BackendFailure` (a Vulkan
    /// call returned an error, whose spelling is the message and whose numeric value is in
    /// the log line beside it).
    [[nodiscard]] static Result<VulkanBackend> Create(IAllocator&   allocator,
                                                      const Config& config);

    /// Destroys the messenger, the instance and the loader, in that order. Safe to call
    /// unconditionally, safe to call more than once, and safe on a backend that has been
    /// moved from. Bringing one back up means calling `Create` again.
    void Shutdown();

    /// True while the `VkInstance` is alive: true on a backend `Create` returned, false after
    /// `Shutdown`, and false on one that has been moved from.
    [[nodiscard]] bool IsInitialized() const;

    /// The version the *loader* reports, from `vkEnumerateInstanceVersion` -- not any
    /// device's. `{0, 0, 0}` after `Shutdown` and on a moved-from backend. On the development
    /// machine this is 1.4.357 while one of the two devices is 1.3.275, which is why the two
    /// numbers are kept apart: it is the device's that decides what may be called on it.
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
    /// Everything this class owns lives behind one pointer, allocated from the caller's
    /// allocator, because the state names Vulkan types and a public header may not
    /// (Docs/Rendering/RHI.md, and ADR-0014's rule 1). The alternative -- opaque fixed-size
    /// storage, as `Platform::Library` uses for one `HMODULE` -- does not scale to three
    /// entry-point tables and a `Platform::Library`: the size would be a number in this
    /// header that a static_assert in the .cpp polices, and every table added later would
    /// change it.
    struct State;

    /// Adopts a state `Create` has already brought up. Private, so the only way to come by a
    /// backend is a `Create` that succeeded.
    explicit VulkanBackend(State* state) noexcept;

    /// Shuts down and then destroys and deallocates the state. What the destructor and the
    /// move-assignment operator share, and the only private member function of this class
    /// that names no Vulkan type -- everything else lives on State, whose definition is in
    /// Private/VulkanBackend.cpp because it names plenty of them.
    void Release();

    /// Null only on a backend that has been moved from.
    State* m_state = nullptr;
};

}  // namespace Monarc::RHI
