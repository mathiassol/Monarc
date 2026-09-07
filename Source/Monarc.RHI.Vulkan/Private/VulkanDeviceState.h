#pragma once

// The types a Vulkan logical device is made of: its three pool-slot structs, its command list,
// its queue, and the state object that owns all of them.
//
// **A private header, because two translation units now need these declarations and one file
// should not hold three jobs.** The A3 plan's file listing names
// `VulkanBackend.cpp  VulkanDevice.cpp  VulkanCommandList.cpp`, and until this header existed
// the third of those did not: command recording lived inside VulkanDevice.cpp, which had grown
// to 1533 lines holding the device, the command list and the queue at once. Splitting the
// implementation needs the declarations somewhere both halves can see, and a `Private/` header
// is that somewhere -- for `Loader.h`'s reason, one directory over: these declarations name
// VkImage, VkCommandBuffer and VkDeviceMemory, and this module's public headers may name no
// Vulkan type at all (Docs/Rendering/RHI.md; ADR-0014 rule 1).
//
// **This header is a departure from the plan's listing in the other direction**, and it is
// recorded here rather than left to be noticed: the plan names no VulkanDeviceState.h. What it
// buys is the third file the plan *does* name, and it buys it without a second copy of any
// declaration -- the alternative was to leave `VulkanCommandList` where it was and let Task 4
// add a swapchain to the same file.
//
// **`VulkanQueue` is declared here and still implemented in VulkanDevice.cpp**, which is a
// judgement and not an oversight. `Submit` stamps `FrameSlot::timelineValue` and
// `IDevice::BeginFrame` is what waits on it and clears it: those are two halves of one
// mechanism, and putting them in two files would separate the only two functions that write
// that field. `VulkanCommandList` has no such tie -- it reads `functions`, `Resolve` and its
// own recording state, and nothing else on the device writes what it writes.

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Log.h>
#include <Monarc/Core/Memory/Allocator.h>
#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Adapter.h>
#include <Monarc/RHI/Device.h>
#include <Monarc/RHI/Handles.h>

#include <Loader.h>
#include <vulkan/vulkan.h>

namespace Monarc::RHI::Detail {

namespace LogCategories {

/// Nested in a namespace of its own for one mechanical reason: MONARC_LOG_CATEGORY declares a
/// *variable* named after the category, and a variable called `VulkanDevice` beside the class
/// of that name would be a redeclaration -- or, in `Detail`, would shadow the class name in
/// every `Result<VulkanDevice>` below it. The nesting keeps the category's spelling, which is
/// what appears in a log line, while leaving the type name alone.
MONARC_LOG_CATEGORY(VulkanDevice, Info);

}  // namespace LogCategories

/// Declared before the two classes that hold a pointer to it and defined at the foot of this
/// header, because the dependency runs both ways: the command list and the queue each need a
/// `VulkanDeviceState*`, and the state has one of each by value. Monarc/RHI/Vulkan/VulkanDevice.h
/// declares the same name for its own reason -- it is what `VulkanDevice::m_state` points at --
/// and a repeated declaration of an incomplete type is exactly that and nothing more.
struct VulkanDeviceState;

/// One texture's pool slot. Generation-checked, per ADR-0002: a slot is reused after its
/// texture is destroyed, so the index alone cannot tell one occupant from the next.
struct TextureSlot {
    VkImage            image       = VK_NULL_HANDLE;
    VkImageView        view        = VK_NULL_HANDLE;
    VkDeviceMemory     memory      = VK_NULL_HANDLE;
    TextureDescription description = {};

    /// Bumped on both halves of a slot's life -- once when it is claimed and once when it is
    /// released -- so that two bumps pass per occupant rather than one. **The release bump is
    /// what makes the generation sufficient on its own to refuse a handle whose resource has
    /// been destroyed**, and that is the whole reason it is there.
    ///
    /// Bumping only on claim leaves a window, from a destroy until the next claim, in which a
    /// destroyed handle's generation still matches its slot -- and in that window `live` is the
    /// only thing refusing it. That was a single point of failure for ADR-0002's guarantee, and
    /// a measured one: with the claim-only bump, dropping the `live` check from `Resolve`
    /// handed a VK_NULL_HANDLE image to vkCmdCopyImageToBuffer2 and stopped the process
    /// ("srcImage is VK_NULL_HANDLE"), with 204 assertions green and none red. With the release
    /// bump in, the same mutation is refused by the generation and turns assertions red instead.
    ///
    /// **`JobSystem::ClaimSlotLocked` bumps on claim only, and that is not a precedent for
    /// doing the same here.** The mechanics match; the safety argument does not. There `done`
    /// is both the free/occupied flag and the semantic answer, a stale job handle's correct
    /// answer is "complete" (`JobSystem::IsCompleteLocked`), and nothing is dereferenced -- so
    /// a wrong flag yields a benign default. Here the stale answer must be "refuse", and a
    /// wrong flag yields a null image. Same shape, opposite failure mode.
    ///
    /// What one bump per occupant bought was tidiness: a slot's generation changed exactly
    /// once per occupant. What it cost was the invariant the counter exists for.
    ///
    /// Zero means never claimed, and no handle a device issued names it -- the first claim
    /// makes it one, which the device test asserts directly. A live slot's generation is
    /// therefore odd and a free slot's even, since claim and release alternate and each moves
    /// it by one; that parity is a consequence of the two bumps rather than a separate rule,
    /// and nothing relies on it.
    ///
    /// The counter is 32 bits and wraps. Two bumps per occupant reach the wrap in 2^31
    /// create/destroy cycles on one slot rather than 2^32 -- **untested, and stated as
    /// arithmetic rather than as a guard**, because provoking it needs those two billion cycles
    /// on a single slot and no test can. It is halved here rather than introduced here: a
    /// wrapped counter collides with a far-later occupant exactly as it did before.
    u32 generation = 0;

    /// Whether this slot has an occupant. An allocator concern: it is what the claim helpers
    /// scan for and what `Shutdown` releases by. `Resolve` consults it too, but only for the
    /// forged-handle case the generation cannot answer -- see `Resolve(TextureHandle)`.
    bool live = false;
};

/// One buffer's pool slot. `TextureSlot`'s two-bump generation rule applies unchanged.
struct BufferSlot {
    VkBuffer          buffer      = VK_NULL_HANDLE;
    VkDeviceMemory    memory      = VK_NULL_HANDLE;
    BufferDescription description = {};

    /// Non-null while `MapBufferForRead` has this buffer mapped. Unmapping is the caller's, and
    /// `Shutdown` unmaps whatever is left rather than freeing memory that is still mapped.
    void* mapped = nullptr;

    u32  generation = 0;
    bool live       = false;
};

/// One of `kFramesInFlight` frame slots: its own command pool, its own command buffer, and the
/// timeline value its last submission will signal.
struct FrameSlot {
    VkCommandPool   pool          = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    /// The value this slot's outstanding submission will signal, or zero when it has none.
    /// `BeginFrame` waits on it before resetting the pool, and zero unambiguously means "there
    /// is nothing to wait for" because the timeline starts at zero and no submission signals
    /// it.
    ///
    /// Stamped by `IQueue::Submit` and cleared by the `BeginFrame` whose wait on it returned,
    /// so the field describes what is still pending rather than what once was. Clearing is
    /// what keeps "zero means nothing to wait for" true of a slot that was begun and never
    /// submitted: without it such a slot carries an already-signalled value and the next wrap
    /// waits on it again -- harmless, since the wait returns at once, but no longer the
    /// meaning the field claims.
    u64 timelineValue = 0;
};

class VulkanCommandList final : public ICommandList {
public:
    VulkanCommandList() = default;

    /// Points this list at its device and frame slot. Called once, by the factory, after the
    /// state exists -- which is why it is a second step rather than a constructor.
    void Attach(VulkanDeviceState* state, u32 frameIndex);

    /// Forgets that anything was recorded. `BeginFrame` calls this after resetting the pool,
    /// so a list left mid-recording by a caller that never called `End` does not carry that
    /// state into the next frame.
    void Reset();

    /// Forgets the command buffer as well as the recording state. `VulkanDeviceState::Shutdown`
    /// calls this after destroying the command pools, which is what makes every recording call
    /// on a shut-down device refuse rather than dispatch through a freed `VkCommandBuffer` --
    /// see that function.
    void Detach();

    [[nodiscard]] VkCommandBuffer Buffer() const { return m_buffer; }
    [[nodiscard]] u32             FrameIndex() const { return m_frameIndex; }
    [[nodiscard]] bool            IsRecording() const { return m_recording; }
    [[nodiscard]] bool            IsRendering() const { return m_rendering; }

    /// Whether `Begin` and then `End` have both run since this list was last reset, so its
    /// command buffer holds a recording a queue may submit.
    ///
    /// **Separate from `!IsRecording()`, and that is the whole point.** A list straight out of
    /// `BeginFrame` is not recording either, because `BeginFrame` reset the pool and returned
    /// its command buffer to Vulkan's initial state -- submitting that is
    /// `VUID-vkQueueSubmit2-commandBuffer-03874` and stops the process. Only this says the
    /// difference between "finished recording" and "never started".
    [[nodiscard]] bool IsRecorded() const { return m_recorded; }

    [[nodiscard]] Status Begin() override;
    [[nodiscard]] Status End() override;
    void                 Barrier(const GlobalBarrier& barrier) override;
    void                 Barrier(const BufferBarrier& barrier) override;
    void                 Barrier(const TextureBarrier& barrier) override;
    [[nodiscard]] Status BeginRendering(const RenderingDescription& description) override;
    void                 EndRendering() override;
    [[nodiscard]] Status CopyTextureToBuffer(TextureHandle source,
                                             BufferHandle  destination) override;

private:
    /// Whether a `Cmd`-style call may be recorded now. Reports through MONARC_CHECK and
    /// returns the answer; the three `Barrier` overloads and `EndRendering` return void, so
    /// this is the only report available to them.
    ///
    /// Void rather than `Status` for those four is deliberate and mirrors the API: Vulkan's own
    /// `vkCmd*` functions return nothing, because a command buffer defers every error it can
    /// to `vkEndCommandBuffer` -- which is exactly what `End()` above surfaces. Recording on a
    /// list that is not recording is not a deferred error, it is a caller bug, and the house
    /// pattern for one is a check.
    [[nodiscard]] bool CanRecord(const char* operation) const;

    VulkanDeviceState* m_state      = nullptr;
    VkCommandBuffer    m_buffer     = VK_NULL_HANDLE;
    u32                m_frameIndex = 0;
    bool               m_recording  = false;
    bool               m_rendering  = false;
    bool               m_recorded   = false;
};

class VulkanQueue final : public IQueue {
public:
    VulkanQueue() = default;

    void Attach(VulkanDeviceState* state) { m_state = state; }

    [[nodiscard]] Result<u64> Submit(ICommandList& commands) override;
    [[nodiscard]] Status      Wait(u64 value, u64 timeoutNanoseconds) override;
    [[nodiscard]] Result<u64> CompletedValue() const override;
    [[nodiscard]] u64         LastSubmittedValue() const override;

private:
    /// Every handle this queue uses lives on the device's state rather than here: there is one
    /// queue and one timeline per device, so a copy on this object would be a second place for
    /// the same fact to live and drift from.
    VulkanDeviceState* m_state = nullptr;
};

struct VulkanDeviceState {
    VulkanDeviceState(IAllocator& allocatorToUse, const DeviceConfig& configToUse)
        : allocator(allocatorToUse),
          config(configToUse),
          textures(allocatorToUse),
          buffers(allocatorToUse) {}

    IAllocator&  allocator;
    DeviceConfig config;

    DeviceFunctions                  functions;
    VkPhysicalDevice                 physicalDevice = VK_NULL_HANDLE;
    VkDevice                         device         = VK_NULL_HANDLE;
    VkQueue                          graphicsQueue  = VK_NULL_HANDLE;
    VkSemaphore                      timeline       = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties{};

    /// The adapter as the driver described it at device creation, not as a caller passed it in
    /// -- `VulkanBackend::CreateDevice` re-queries. So `IDevice::Adapter()` is a fact about the
    /// device rather than an echo.
    AdapterInfo adapter{};

    u32 graphicsQueueFamily = 0;

    /// Highest value any submission on this queue will signal. Monotonic, one per submission.
    u64 lastSubmittedValue = 0;

    /// Which frame slot `BeginFrame` last handed out. Starts one below zero, modularly, so the
    /// first `BeginFrame` returns slot 0.
    u32 frameIndex = kFramesInFlight - 1;

    FrameSlot         frames[kFramesInFlight];
    VulkanQueue       queue;
    VulkanCommandList lists[kFramesInFlight];

    /// Fixed-capacity resource pools, `JobSystem`'s discipline: filled to capacity once at
    /// creation and never pushed to again, so no creation call can reallocate them and
    /// invalidate a handle's slot address. A full pool reports `ErrorCode::OutOfMemory`.
    Array<TextureSlot> textures;
    Array<BufferSlot>  buffers;

    [[nodiscard]] std::unexpected<Error> FailVk(const char* operation, VkResult result);

    [[nodiscard]] TextureSlot* Resolve(TextureHandle handle);
    [[nodiscard]] BufferSlot*  Resolve(BufferHandle handle);

    /// The `VulkanCommandList` `list` points at, if it is one of this device's own.
    ///
    /// **Address comparison and not a downcast, which is what makes it safe.** Casting an
    /// `ICommandList&` of unknown dynamic type to `VulkanCommandList&` is undefined before
    /// there is anything left to check, and `dynamic_cast` would need RTTI. Comparing the
    /// argument's address against this device's own lists is well defined for any pointer, and
    /// it answers the question that can actually go wrong on this machine: two adapters mean
    /// two devices, and a list from one submitted to the other's queue is refused here.
    [[nodiscard]] VulkanCommandList* FindOwnList(ICommandList* list);

    [[nodiscard]] u32 ClaimTextureSlot();
    [[nodiscard]] u32 ClaimBufferSlot();

    [[nodiscard]] Result<VkDeviceMemory> AllocateFor(const VkMemoryRequirements& requirements,
                                                     MemoryLocation              location);

    void ReleaseTextureSlot(TextureSlot& slot);
    void ReleaseBufferSlot(BufferSlot& slot);

    void Shutdown();
};

}  // namespace Monarc::RHI::Detail
