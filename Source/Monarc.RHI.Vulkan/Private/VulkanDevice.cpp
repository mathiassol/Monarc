// The logical device, its queue and timeline, its resource pools, and command recording.
//
// **No VkRenderPass and no VkFramebuffer, here or anywhere else in this module.** Rendering
// begins with vkCmdBeginRendering against image views named at record time -- Vulkan 1.3
// dynamic rendering, which is the model D3D12 and Metal already have. Nothing is created ahead
// of a frame, nothing has to be invalidated when a texture is recreated, and there are no
// pipeline/pass compatibility rules to encode.
//
// The claim is checkable rather than asserted: `grep -rni "renderpass\|framebuffer\|render
// pass" Source/` matches four lines, all of them comments -- two here and two in
// Monarc/RHI/Device.h -- and no code. Under VK_NO_PROTOTYPES a call could only reach one of
// those entry points through Private/Loader.h's tables, which name none of them.
//
// **Memory is one VkDeviceMemory per resource, and that is a placeholder.** It is honest for
// A3, which creates one texture and one buffer, and it will not survive the first scene with
// real assets: a driver's allocation count is limited (maxMemoryAllocationCount, 4096 on much
// hardware), each allocation has real cost, and nothing here sub-allocates or pools. What
// makes the placeholder safe to ship is the shape of the interface rather than the strategy
// behind it: `CreateTexture` takes a `TextureDescription` and returns a handle, so a
// sub-allocating allocator replaces the body of one function without touching a call site.
// ADR-0014's dependency table has no VMA row, and adding one is not this task's decision --
// see Docs/Status.md, which records this as a known non-scalable placeholder.
//
// Almost everything here is a member of `Detail::VulkanDeviceState` rather than of
// `VulkanDevice`, for VulkanBackend.cpp's reason: these functions name VkImage, VkDeviceMemory
// and VkCommandBuffer, and this module's public headers may name no Vulkan type at all
// (Docs/Rendering/RHI.md; ADR-0014 rule 1). The public class is the Vulkan-free shell in front
// of them.

#include <Monarc/RHI/Vulkan/VulkanDevice.h>

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Log.h>

#include <ArrayOps.h>
#include <Loader.h>
#include <Translate.h>
#include <VulkanDeviceFactory.h>

#include <new>
#include <utility>

namespace Monarc::RHI::Detail {

namespace LogCategories {

/// Nested in a namespace of its own for one mechanical reason: MONARC_LOG_CATEGORY declares a
/// *variable* named after the category, and a variable called `VulkanDevice` beside the class
/// of that name would be a redeclaration -- or, in `Detail`, would shadow the class name in
/// every `Result<VulkanDevice>` below it. The nesting keeps the category's spelling, which is
/// what appears in a log line, while leaving the type name alone.
MONARC_LOG_CATEGORY(VulkanDevice, Info);

}  // namespace LogCategories

namespace {

/// How long `IDevice::BeginFrame` and `IQueue::Wait`'s callers wait on the timeline before
/// giving up.
///
/// Five seconds, and it is a deadlock detector rather than a performance budget: a frame's
/// worth of work on either local adapter is microseconds, so anything past a second means the
/// submission is never completing. A timeout is reported as a failure and never as success --
/// see `IQueue::Wait` -- because a caller that read the buffer anyway would be reading memory
/// the GPU is still writing.
///
/// `UINT64_MAX` was the alternative and is worse: a test suite that hangs forever tells CI
/// nothing, where one that fails names the submission that never finished.
constexpr u64 kTimelineWaitTimeoutNanoseconds = 5'000'000'000ULL;

/// No pool slot ever has this index. Returned by the claim helpers when a pool is full, and
/// the same convention `JobSystem::kNoSlot` uses.
constexpr u32 kNoSlot = static_cast<u32>(-1);

}  // namespace

/// One texture's pool slot. Generation-checked, per ADR-0002: a slot is reused after its
/// texture is destroyed, so the index alone cannot tell one occupant from the next.
struct TextureSlot {
    VkImage            image       = VK_NULL_HANDLE;
    VkImageView        view        = VK_NULL_HANDLE;
    VkDeviceMemory     memory      = VK_NULL_HANDLE;
    TextureDescription description = {};

    /// Bumped every time this slot is claimed, so a handle from a previous occupant does not
    /// match. Starts at zero and the first claim makes it one, which is what keeps generation
    /// zero from ever naming a live resource.
    u32 generation = 0;

    bool live = false;
};

/// One buffer's pool slot. `TextureSlot`'s generation rule applies unchanged.
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

    /// Zero until this slot has been submitted once. `BeginFrame` waits on it before resetting
    /// the pool, and zero unambiguously means "never submitted" because the timeline starts at
    /// zero and no submission signals it.
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

    [[nodiscard]] VkCommandBuffer Buffer() const { return m_buffer; }
    [[nodiscard]] u32             FrameIndex() const { return m_frameIndex; }
    [[nodiscard]] bool            IsRecording() const { return m_recording; }
    [[nodiscard]] bool            IsRendering() const { return m_rendering; }

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

// ---------------------------------------------------------------------------------------
// VulkanDeviceState
// ---------------------------------------------------------------------------------------

std::unexpected<Error> VulkanDeviceState::FailVk(const char* operation, VkResult result) {
    // VulkanBackend::State::FailVk's shape and its reasoning: the message is the VkResult's own
    // spelling, which is a pointer to a literal and so outlives this state, and the
    // composition -- which call, and the numeric value -- goes to the log beside it.
    MONARC_LOG(LogCategories::VulkanDevice, Warning, "{} failed: {} ({})", operation,
               ToString(result), static_cast<i32>(result));
    return Err(ToErrorCode(result), ToString(result));
}

TextureSlot* VulkanDeviceState::Resolve(TextureHandle handle) {
    if (!handle.IsValid() || handle.index >= textures.Size()) {
        return nullptr;
    }
    TextureSlot& slot = textures[handle.index];
    if (!slot.live || slot.generation != handle.generation) {
        return nullptr;
    }
    return &slot;
}

BufferSlot* VulkanDeviceState::Resolve(BufferHandle handle) {
    if (!handle.IsValid() || handle.index >= buffers.Size()) {
        return nullptr;
    }
    BufferSlot& slot = buffers[handle.index];
    if (!slot.live || slot.generation != handle.generation) {
        return nullptr;
    }
    return &slot;
}

VulkanCommandList* VulkanDeviceState::FindOwnList(ICommandList* list) {
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        if (static_cast<ICommandList*>(&lists[i]) == list) {
            return &lists[i];
        }
    }
    return nullptr;
}

u32 VulkanDeviceState::ClaimTextureSlot() {
    for (usize index = 0; index < textures.Size(); ++index) {
        if (!textures[index].live) {
            // Bumped on claim, not on release, so that a slot's generation changes exactly
            // once per occupant and generation zero never names a live one. JobSystem's
            // ClaimSlotLocked does the same, for the same reason.
            ++textures[index].generation;
            textures[index].live = true;
            return static_cast<u32>(index);
        }
    }
    return kNoSlot;
}

u32 VulkanDeviceState::ClaimBufferSlot() {
    for (usize index = 0; index < buffers.Size(); ++index) {
        if (!buffers[index].live) {
            ++buffers[index].generation;
            buffers[index].live = true;
            return static_cast<u32>(index);
        }
    }
    return kNoSlot;
}

Result<VkDeviceMemory> VulkanDeviceState::AllocateFor(const VkMemoryRequirements& requirements,
                                                      MemoryLocation              location) {
    const VkMemoryPropertyFlags required = ToVulkan(location);
    const u32 typeIndex = FindMemoryType(memoryProperties, requirements.memoryTypeBits, required);
    if (typeIndex == kNoMemoryType) {
        MONARC_LOG(LogCategories::VulkanDevice, Warning,
                   "no memory type on \"{}\" is both usable for this resource (mask 0x{:x}) and "
                   "has the required properties (0x{:x})",
                   adapter.name, requirements.memoryTypeBits, static_cast<u32>(required));
        return Err(ErrorCode::Unsupported,
                   "this device has no memory type with the properties the resource needs");
    }

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize  = requirements.size;
    allocateInfo.memoryTypeIndex = typeIndex;

    // One allocation per resource. See the file comment: a placeholder, and the interface
    // rather than this line is what makes it replaceable.
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (const VkResult result =
            functions.vkAllocateMemory(device, &allocateInfo, nullptr, &memory);
        result != VK_SUCCESS) {
        return FailVk("vkAllocateMemory", result);
    }
    return memory;
}

void VulkanDeviceState::ReleaseTextureSlot(TextureSlot& slot) {
    // Reverse creation order: the view addresses the image, and the image is bound to the
    // memory.
    if (slot.view != VK_NULL_HANDLE) {
        functions.vkDestroyImageView(device, slot.view, nullptr);
        slot.view = VK_NULL_HANDLE;
    }
    if (slot.image != VK_NULL_HANDLE) {
        functions.vkDestroyImage(device, slot.image, nullptr);
        slot.image = VK_NULL_HANDLE;
    }
    if (slot.memory != VK_NULL_HANDLE) {
        functions.vkFreeMemory(device, slot.memory, nullptr);
        slot.memory = VK_NULL_HANDLE;
    }
    slot.description = TextureDescription{};
    slot.live        = false;
}

void VulkanDeviceState::ReleaseBufferSlot(BufferSlot& slot) {
    if (slot.mapped != nullptr) {
        // Freeing memory that is still mapped is legal in Vulkan but leaves the caller holding
        // a span into unmapped pages. Unmapping here means a caller that forgot cannot be
        // handed one.
        functions.vkUnmapMemory(device, slot.memory);
        slot.mapped = nullptr;
    }
    if (slot.buffer != VK_NULL_HANDLE) {
        functions.vkDestroyBuffer(device, slot.buffer, nullptr);
        slot.buffer = VK_NULL_HANDLE;
    }
    if (slot.memory != VK_NULL_HANDLE) {
        functions.vkFreeMemory(device, slot.memory, nullptr);
        slot.memory = VK_NULL_HANDLE;
    }
    slot.description = BufferDescription{};
    slot.live        = false;
}

void VulkanDeviceState::Shutdown() {
    if (device == VK_NULL_HANDLE) {
        return;
    }

    // Idle first, and it is not a convenience. Destroying a command pool whose buffers are
    // still executing, or an image a submitted copy is still reading, is undefined behaviour
    // and a validation error -- which, with the fatal messenger installed, stops the process.
    //
    // Guarded because the factory's failure path reaches here: `LoadDeviceFunctions` clears the
    // whole table when a Required entry is missing, so a device can exist with an empty table.
    // Exactly the shape VulkanBackend::State::Shutdown guards vkDestroyInstance for, and that
    // one was measured: without the guard, a forced-null entry point exited 0xC0000005.
    if (functions.vkDeviceWaitIdle != nullptr) {
        if (const VkResult result = functions.vkDeviceWaitIdle(device); result != VK_SUCCESS) {
            // Nothing can be done about it, and pressing on is right: the alternative is
            // leaking the whole device. A lost device is the realistic cause.
            MONARC_LOG(LogCategories::VulkanDevice, Error,
                       "vkDeviceWaitIdle failed during shutdown: {} ({}); destroying anyway",
                       ToString(result), static_cast<i32>(result));
        }
    }

    // Everything below can only be non-null if the device table resolved, because the factory
    // creates all of it after `LoadDeviceFunctions` has already succeeded. So these need no
    // guards of their own -- only `vkDestroyDevice` does, being reachable with an empty table.
    for (TextureSlot& slot : textures) {
        if (slot.live) {
            ReleaseTextureSlot(slot);
        }
    }
    for (BufferSlot& slot : buffers) {
        if (slot.live) {
            ReleaseBufferSlot(slot);
        }
    }

    for (FrameSlot& slot : frames) {
        if (slot.pool != VK_NULL_HANDLE) {
            // The pool owns its command buffers, so destroying it frees them -- which is why
            // vkFreeCommandBuffers is not in the device table at all.
            functions.vkDestroyCommandPool(device, slot.pool, nullptr);
            slot.pool          = VK_NULL_HANDLE;
            slot.commandBuffer = VK_NULL_HANDLE;
        }
        slot.timelineValue = 0;
    }

    if (timeline != VK_NULL_HANDLE) {
        functions.vkDestroySemaphore(device, timeline, nullptr);
        timeline = VK_NULL_HANDLE;
    }

    if (functions.vkDestroyDevice != nullptr) {
        functions.vkDestroyDevice(device, nullptr);
    } else {
        MONARC_LOG(LogCategories::VulkanDevice, Error,
                   "the Vulkan device cannot be destroyed: vkDestroyDevice never resolved, so "
                   "it is leaked until the process exits");
    }
    device        = VK_NULL_HANDLE;
    graphicsQueue = VK_NULL_HANDLE;

    lastSubmittedValue = 0;
    frameIndex         = kFramesInFlight - 1;
}

// ---------------------------------------------------------------------------------------
// VulkanCommandList
// ---------------------------------------------------------------------------------------

void VulkanCommandList::Attach(VulkanDeviceState* state, u32 frameIndex) {
    m_state      = state;
    m_frameIndex = frameIndex;
    m_buffer     = state->frames[frameIndex].commandBuffer;
    m_recording  = false;
    m_rendering  = false;
}

void VulkanCommandList::Reset() {
    m_recording = false;
    m_rendering = false;
}

bool VulkanCommandList::CanRecord(const char* operation) const {
    if (!m_recording) {
        MONARC_CHECK(false, operation);
        return false;
    }
    return true;
}

Status VulkanCommandList::Begin() {
    if (m_state == nullptr || m_buffer == VK_NULL_HANDLE) {
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::Begin called on a list that belongs to no device");
    }
    if (m_recording) {
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::Begin called on a list that is already recording");
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    // ONE_TIME_SUBMIT, because that is what this is: `BeginFrame` resets the whole pool before
    // handing the list back, so a buffer is never submitted twice. Telling the driver so is
    // free and lets it discard the recording after submission.
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (const VkResult result = m_state->functions.vkBeginCommandBuffer(m_buffer, &beginInfo);
        result != VK_SUCCESS) {
        return m_state->FailVk("vkBeginCommandBuffer", result);
    }
    m_recording = true;
    m_rendering = false;
    return {};
}

Status VulkanCommandList::End() {
    if (!m_recording) {
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::End called on a list that is not recording");
    }
    if (m_rendering) {
        // Ending a command buffer inside a rendering pass is a validation error, so catching
        // it here turns a process-stopping abort into a returned Status. The list stays
        // recording, because it is: nothing has been ended.
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::End called inside a rendering pass; call EndRendering first");
    }

    if (const VkResult result = m_state->functions.vkEndCommandBuffer(m_buffer);
        result != VK_SUCCESS) {
        return m_state->FailVk("vkEndCommandBuffer", result);
    }
    m_recording = false;
    return {};
}

void VulkanCommandList::Barrier(const GlobalBarrier& barrier) {
    if (!CanRecord("ICommandList::Barrier(GlobalBarrier) on a list that is not recording")) {
        return;
    }

    // Recorded even when nothing changes. `GlobalBarrier{}` is a value a caller can mean --
    // Barrier.h says why -- and dropping it here would be this function deciding on the
    // caller's behalf with less information than the caller had.
    const VkMemoryBarrier2 translated = ToVulkan(barrier);

    VkDependencyInfo dependency{};
    dependency.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers    = &translated;
    m_state->functions.vkCmdPipelineBarrier2(m_buffer, &dependency);
}

void VulkanCommandList::Barrier(const BufferBarrier& barrier) {
    if (!CanRecord("ICommandList::Barrier(BufferBarrier) on a list that is not recording")) {
        return;
    }
    const BufferSlot* slot = m_state->Resolve(barrier.buffer);
    if (slot == nullptr) {
        MONARC_CHECK(false,
                     "ICommandList::Barrier(BufferBarrier) names a buffer this device does "
                     "not have, or one whose handle is stale");
        return;
    }

    const VkBufferMemoryBarrier2 translated = ToVulkan(barrier, slot->buffer);

    VkDependencyInfo dependency{};
    dependency.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers    = &translated;
    m_state->functions.vkCmdPipelineBarrier2(m_buffer, &dependency);
}

void VulkanCommandList::Barrier(const TextureBarrier& barrier) {
    if (!CanRecord("ICommandList::Barrier(TextureBarrier) on a list that is not recording")) {
        return;
    }
    const TextureSlot* slot = m_state->Resolve(barrier.Texture());
    if (slot == nullptr) {
        MONARC_CHECK(false,
                     "ICommandList::Barrier(TextureBarrier) names a texture this device does "
                     "not have, or one whose handle is stale");
        return;
    }

    const VkImageMemoryBarrier2 translated = ToVulkan(barrier, slot->image);

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &translated;
    m_state->functions.vkCmdPipelineBarrier2(m_buffer, &dependency);
}

Status VulkanCommandList::BeginRendering(const RenderingDescription& description) {
    if (!m_recording) {
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::BeginRendering called on a list that is not recording");
    }
    if (m_rendering) {
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::BeginRendering called inside a rendering pass");
    }
    if (description.extent.IsEmpty()) {
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::BeginRendering was given an empty render extent");
    }
    if (description.colorAttachments.size() > kMaxColorAttachments) {
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::BeginRendering was given more colour attachments than "
                   "kMaxColorAttachments");
    }

    VkRenderingAttachmentInfo attachments[kMaxColorAttachments]{};
    for (usize i = 0; i < description.colorAttachments.size(); ++i) {
        const ColorAttachment& attachment = description.colorAttachments[i];
        const TextureSlot*     slot       = m_state->Resolve(attachment.texture);
        if (slot == nullptr) {
            // The generation check is the whole point of ADR-0002: a handle naming a slot that
            // has been recycled resolves to a failure rather than to whatever now occupies it.
            return Err(ErrorCode::InvalidArgument,
                       "ICommandList::BeginRendering names a texture this device does not "
                       "have, or one whose handle is stale");
        }
        if (slot->view == VK_NULL_HANDLE) {
            // A texture created without `TextureUsage::ColorAttachment` has no image view, and
            // dynamic rendering needs one -- see `CreateTexture`, where the eager version of
            // that view was a validation error on a transfer-only texture. Refusing here names
            // the missing usage; passing VK_NULL_HANDLE through would be
            // VUID-VkRenderingInfo-colorAttachmentCount-06087 and would stop the process.
            return Err(ErrorCode::InvalidArgument,
                       "ICommandList::BeginRendering names a texture created without "
                       "TextureUsage::ColorAttachment, so it has no view to render into");
        }

        attachments[i].sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachments[i].imageView   = slot->view;
        // The caller's barrier is what put it here; RenderingDescription says so. Naming the
        // layout rather than reading it back means this function tracks no per-texture state,
        // which is what keeps the render graph's job the render graph's.
        attachments[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[i].resolveMode = VK_RESOLVE_MODE_NONE;
        attachments[i].loadOp      = ToVulkan(attachment.loadOp);
        attachments[i].storeOp     = ToVulkan(attachment.storeOp);
        attachments[i].clearValue.color.float32[0] = attachment.clearValue.r;
        attachments[i].clearValue.color.float32[1] = attachment.clearValue.g;
        attachments[i].clearValue.color.float32[2] = attachment.clearValue.b;
        attachments[i].clearValue.color.float32[3] = attachment.clearValue.a;
    }

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea.offset    = VkOffset2D{0, 0};
    renderingInfo.renderArea.extent    = VkExtent2D{description.extent.width,
                                                    description.extent.height};
    renderingInfo.layerCount           = 1;
    renderingInfo.colorAttachmentCount = static_cast<u32>(description.colorAttachments.size());
    renderingInfo.pColorAttachments =
        description.colorAttachments.empty() ? nullptr : attachments;

    m_state->functions.vkCmdBeginRendering(m_buffer, &renderingInfo);
    m_rendering = true;
    return {};
}

void VulkanCommandList::EndRendering() {
    if (!CanRecord("ICommandList::EndRendering on a list that is not recording")) {
        return;
    }
    if (!m_rendering) {
        MONARC_CHECK(false, "ICommandList::EndRendering with no rendering pass begun");
        return;
    }
    m_state->functions.vkCmdEndRendering(m_buffer);
    m_rendering = false;
}

Status VulkanCommandList::CopyTextureToBuffer(TextureHandle source, BufferHandle destination) {
    if (!m_recording) {
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::CopyTextureToBuffer called on a list that is not recording");
    }
    if (m_rendering) {
        // A copy inside a rendering pass is a validation error. Returning is better than
        // recording it and having the messenger stop the process.
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::CopyTextureToBuffer called inside a rendering pass");
    }

    const TextureSlot* texture = m_state->Resolve(source);
    if (texture == nullptr) {
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::CopyTextureToBuffer's source texture is not one this device "
                   "has, or its handle is stale");
    }
    const BufferSlot* buffer = m_state->Resolve(destination);
    if (buffer == nullptr) {
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::CopyTextureToBuffer's destination buffer is not one this "
                   "device has, or its handle is stale");
    }

    const u64 required = static_cast<u64>(texture->description.extent.width) *
                         texture->description.extent.height *
                         BytesPerPixel(texture->description.format);
    if (buffer->description.size < required) {
        MONARC_LOG(LogCategories::VulkanDevice, Warning,
                   "the destination buffer holds {} bytes and the texture needs {}",
                   buffer->description.size, required);
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::CopyTextureToBuffer's destination buffer is smaller than the "
                   "texture it would receive");
    }

    VkBufferImageCopy2 region{};
    region.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
    region.bufferOffset = 0;
    // Zero for both means "tightly packed", which is what CopyTextureToBuffer promises: rows
    // follow one another with no padding, so the reader's arithmetic is width * bytesPerPixel.
    region.bufferRowLength                 = 0;
    region.bufferImageHeight               = 0;
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset                     = VkOffset3D{0, 0, 0};
    region.imageExtent = VkExtent3D{texture->description.extent.width,
                                    texture->description.extent.height, 1};

    VkCopyImageToBufferInfo2 copyInfo{};
    copyInfo.sType          = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
    copyInfo.srcImage       = texture->image;
    copyInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    copyInfo.dstBuffer      = buffer->buffer;
    copyInfo.regionCount    = 1;
    copyInfo.pRegions       = &region;

    m_state->functions.vkCmdCopyImageToBuffer2(m_buffer, &copyInfo);
    return {};
}

// ---------------------------------------------------------------------------------------
// VulkanQueue
// ---------------------------------------------------------------------------------------

Result<u64> VulkanQueue::Submit(ICommandList& commands) {
    if (m_state == nullptr || m_state->device == VK_NULL_HANDLE) {
        return Err(ErrorCode::InvalidArgument,
                   "IQueue::Submit called on a queue whose device has been shut down");
    }

    VulkanCommandList* list = m_state->FindOwnList(&commands);
    if (list == nullptr) {
        return Err(ErrorCode::InvalidArgument,
                   "IQueue::Submit was given a command list that does not belong to this "
                   "queue's device");
    }
    if (list->IsRecording()) {
        return Err(ErrorCode::InvalidArgument,
                   "IQueue::Submit was given a command list that is still recording; call End "
                   "first");
    }

    const u64 signalValue = m_state->lastSubmittedValue + 1;

    VkCommandBufferSubmitInfo bufferInfo{};
    bufferInfo.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    bufferInfo.commandBuffer = list->Buffer();

    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = m_state->timeline;
    signalInfo.value     = signalValue;
    // ALL_COMMANDS, because the value means "everything in this submission finished". A
    // narrower stage would signal earlier than the promise `Submit` documents.
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount   = 1;
    submitInfo.pCommandBufferInfos      = &bufferInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos    = &signalInfo;

    // No fence. The timeline semaphore is the completion signal, and a fence beside it would be
    // a second answer to one question -- see IQueue's comment on why there are no binary
    // semaphores either.
    if (const VkResult result =
            m_state->functions.vkQueueSubmit2(m_state->graphicsQueue, 1, &submitInfo,
                                              VK_NULL_HANDLE);
        result != VK_SUCCESS) {
        // lastSubmittedValue is deliberately not advanced on failure: nothing will signal
        // signalValue, so a later Wait on it would time out rather than return.
        return m_state->FailVk("vkQueueSubmit2", result);
    }

    m_state->lastSubmittedValue = signalValue;
    // Stamped on the frame slot the list came from, which is what BeginFrame waits on before
    // resetting that slot's pool. The queue is the only thing that knows the value, and the
    // list is the only thing that knows the slot, so this is where the two meet.
    m_state->frames[list->FrameIndex()].timelineValue = signalValue;
    return signalValue;
}

Status VulkanQueue::Wait(u64 value, u64 timeoutNanoseconds) {
    if (m_state == nullptr || m_state->device == VK_NULL_HANDLE) {
        return Err(ErrorCode::InvalidArgument,
                   "IQueue::Wait called on a queue whose device has been shut down");
    }

    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores    = &m_state->timeline;
    waitInfo.pValues        = &value;

    const VkResult result =
        m_state->functions.vkWaitSemaphores(m_state->device, &waitInfo, timeoutNanoseconds);
    if (result == VK_TIMEOUT) {
        // A failure and not a success, which is the whole point: a caller told "fine" would go
        // on to read memory the GPU is still writing.
        MONARC_LOG(LogCategories::VulkanDevice, Error,
                   "waiting for timeline value {} timed out after {} ns (the GPU has reached "
                   "at most {})",
                   value, timeoutNanoseconds, m_state->lastSubmittedValue);
        return Err(ErrorCode::BackendFailure,
                   "waiting for the GPU timeline timed out; the submission never completed");
    }
    if (result != VK_SUCCESS) {
        return m_state->FailVk("vkWaitSemaphores", result);
    }
    return {};
}

Result<u64> VulkanQueue::CompletedValue() const {
    if (m_state == nullptr || m_state->device == VK_NULL_HANDLE) {
        return Err(ErrorCode::InvalidArgument,
                   "IQueue::CompletedValue called on a queue whose device has been shut down");
    }
    u64 value = 0;
    if (const VkResult result = m_state->functions.vkGetSemaphoreCounterValue(
            m_state->device, m_state->timeline, &value);
        result != VK_SUCCESS) {
        return m_state->FailVk("vkGetSemaphoreCounterValue", result);
    }
    return value;
}

u64 VulkanQueue::LastSubmittedValue() const {
    return m_state != nullptr ? m_state->lastSubmittedValue : 0;
}

// ---------------------------------------------------------------------------------------
// VulkanDeviceFactory
// ---------------------------------------------------------------------------------------

namespace {

/// Index of the first queue family on `physicalDevice` that can do graphics, or `kNoSlot`.
///
/// First and not best: A3 has one queue and submits everything to it, so there is nothing to
/// optimise between families. A transfer-only or async-compute family is a later phase's
/// choice, and it will want this to become a query over all families rather than a first hit.
[[nodiscard]] u32 FindGraphicsQueueFamily(const Loader& loader, IAllocator& allocator,
                                          VkPhysicalDevice physicalDevice) {
    u32 count = 0;
    loader.Instance().vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);
    if (count == 0) {
        return kNoSlot;
    }

    Array<VkQueueFamilyProperties> families(allocator);
    ResizeTo(families, count);
    loader.Instance().vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count,
                                                               families.Data());

    for (u32 index = 0; index < count; ++index) {
        if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            return index;
        }
    }
    return kNoSlot;
}

/// Creates the `VkDevice`, one graphics queue, the timeline semaphore and the per-frame command
/// pools into `state`, which must already know its adapter and its queue family.
///
/// Separate from `Create` below only so that `Create`'s allocation-and-cleanup frame stays
/// legible: every failure here is a `Status`, and `Create` runs one teardown path for all of
/// them.
[[nodiscard]] Status BringUpDevice(VulkanDeviceState& state, const Loader& loader) {
    const f32 queuePriority = 1.0F;

    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = state.graphicsQueueFamily;
    queueInfo.queueCount       = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    // The three Vulkan 1.3 core features Monarc's renderer is written against, requested by
    // name. Requesting them is what makes them usable: a device that reports a feature bit
    // still refuses the call unless the feature was enabled at device creation.
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.timelineSemaphore = VK_TRUE;
    features12.pNext             = &features13;

    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features12;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext                = &features;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos    = &queueInfo;
    // **No device extension, at all.** Dynamic rendering, synchronization2, timeline
    // semaphores and copy_commands2 are every one of them promoted to Vulkan 1.3 core, which is
    // Monarc's floor -- so the extension list is empty rather than short. `pEnabledFeatures`
    // stays null because VkPhysicalDeviceFeatures2 is chained instead; Vulkan forbids both.
    deviceInfo.enabledExtensionCount = 0;

    if (const VkResult result = loader.Instance().vkCreateDevice(
            state.physicalDevice, &deviceInfo, nullptr, &state.device);
        result != VK_SUCCESS) {
        state.device = VK_NULL_HANDLE;
        return state.FailVk("vkCreateDevice", result);
    }

    if (Status loaded = loader.LoadDeviceFunctions(state.device, state.functions); !loaded) {
        return loaded;
    }

    state.functions.vkGetDeviceQueue(state.device, state.graphicsQueueFamily, 0,
                                     &state.graphicsQueue);

    VkSemaphoreTypeCreateInfo timelineType{};
    timelineType.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    // Zero, and `FrameSlot::timelineValue` depends on it: no submission ever signals zero, so
    // zero means "this slot has never been submitted" with no separate flag to keep in step.
    timelineType.initialValue = 0;

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreInfo.pNext = &timelineType;

    if (const VkResult result = state.functions.vkCreateSemaphore(state.device, &semaphoreInfo,
                                                                  nullptr, &state.timeline);
        result != VK_SUCCESS) {
        state.timeline = VK_NULL_HANDLE;
        return state.FailVk("vkCreateSemaphore", result);
    }

    for (u32 frame = 0; frame < kFramesInFlight; ++frame) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = state.graphicsQueueFamily;
        // No RESET_COMMAND_BUFFER flag: buffers are never reset individually. BeginFrame resets
        // the whole pool, which is both cheaper and the only reset this design performs -- one
        // pool per frame is what makes that safe.
        poolInfo.flags = 0;

        if (const VkResult result = state.functions.vkCreateCommandPool(
                state.device, &poolInfo, nullptr, &state.frames[frame].pool);
            result != VK_SUCCESS) {
            state.frames[frame].pool = VK_NULL_HANDLE;
            return state.FailVk("vkCreateCommandPool", result);
        }

        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool        = state.frames[frame].pool;
        allocateInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;

        if (const VkResult result = state.functions.vkAllocateCommandBuffers(
                state.device, &allocateInfo, &state.frames[frame].commandBuffer);
            result != VK_SUCCESS) {
            state.frames[frame].commandBuffer = VK_NULL_HANDLE;
            return state.FailVk("vkAllocateCommandBuffers", result);
        }
    }

    return {};
}

}  // namespace

Result<VulkanDevice> VulkanDeviceFactory::Create(IAllocator& allocator, const Loader& loader,
                                                 VkPhysicalDevice    physicalDevice,
                                                 const AdapterInfo&  adapter,
                                                 const DeviceConfig& config) {
    if (config.maxTextures == 0 || config.maxBuffers == 0) {
        // A pool of zero is not a device with no resources, it is a device on which nothing can
        // ever be created -- which is a configuration mistake and not a state worth supporting.
        return Err(ErrorCode::InvalidArgument,
                   "DeviceConfig::maxTextures and maxBuffers must both be at least one");
    }

    const u32 queueFamily = FindGraphicsQueueFamily(loader, allocator, physicalDevice);
    if (queueFamily == kNoSlot) {
        // Reachable only on a device whose capabilities said it had a graphics family and then
        // did not, since VulkanBackend::CreateDevice checks the tier first. Kept because the
        // two facts come from two different queries.
        return Err(ErrorCode::Unsupported,
                   "this device reports no queue family that can do graphics");
    }

    // Allocated before anything is constructed, so a device with no state is never handed to a
    // caller -- VulkanBackend::Create's shape and its reasoning.
    void* storage = allocator.Allocate(sizeof(VulkanDeviceState), alignof(VulkanDeviceState));
    if (storage == nullptr) {
        MONARC_LOG(LogCategories::VulkanDevice, Warning,
                   "the allocator returned nothing for {} bytes of Vulkan device state",
                   sizeof(VulkanDeviceState));
        return Err(ErrorCode::OutOfMemory,
                   "the allocator returned nothing for the Vulkan device's state");
    }
    VulkanDeviceState* state = ::new (storage) VulkanDeviceState(allocator, config);

    state->physicalDevice      = physicalDevice;
    state->adapter             = adapter;
    state->graphicsQueueFamily = queueFamily;
    loader.Instance().vkGetPhysicalDeviceMemoryProperties(physicalDevice,
                                                          &state->memoryProperties);

    if (Status brought = BringUpDevice(*state, loader); !brought) {
        // Nothing half-built survives a failed Create. The Error is copied out before the state
        // is destroyed, which is the reason every message on this path is a string literal:
        // the view has to still point at something once this storage is gone.
        const Error error = brought.error();
        state->Shutdown();
        state->~VulkanDeviceState();
        allocator.Deallocate(storage, sizeof(VulkanDeviceState), alignof(VulkanDeviceState));
        return std::unexpected(error);
    }

    // Filled to capacity here and never pushed to again, which is what makes a slot's address
    // stable for the device's whole life. Array<T> aborts rather than returning on an
    // allocation failure (see Array<T>::OnAllocationFailed), so there is no error for these
    // two lines to report -- which is why they are not part of BringUpDevice's Status chain.
    ResizeTo(state->textures, config.maxTextures);
    ResizeTo(state->buffers, config.maxBuffers);

    // The queue and the lists point back at the state, so they can only be wired up once it
    // exists. That is what makes `Attach` a second step rather than a constructor parameter.
    state->queue.Attach(state);
    for (u32 frame = 0; frame < kFramesInFlight; ++frame) {
        state->lists[frame].Attach(state, frame);
    }

    MONARC_LOG(LogCategories::VulkanDevice, Info,
               "Vulkan device created on \"{}\" | tier {} | graphics queue family {} | {} "
               "frame(s) in flight | pools: {} texture(s), {} buffer(s)",
               adapter.name, ToString(adapter.tier), queueFamily, kFramesInFlight,
               config.maxTextures, config.maxBuffers);
    return VulkanDevice(state);
}

}  // namespace Monarc::RHI::Detail

namespace Monarc::RHI {

VulkanDevice::VulkanDevice(Detail::VulkanDeviceState* state) noexcept : m_state(state) {}

VulkanDevice::~VulkanDevice() { Release(); }

VulkanDevice::VulkanDevice(VulkanDevice&& other) noexcept
    : IDevice(std::move(other)), m_state(std::exchange(other.m_state, nullptr)) {}

VulkanDevice& VulkanDevice::operator=(VulkanDevice&& other) noexcept {
    // Self-assignment would otherwise release this device's state and then adopt the pointer it
    // had just freed -- VulkanBackend::operator='s guard, for the same reason.
    if (this != &other) {
        Release();
        IDevice::operator=(std::move(other));
        m_state = std::exchange(other.m_state, nullptr);
    }
    return *this;
}

void VulkanDevice::Release() {
    if (m_state == nullptr) {
        return;
    }
    m_state->Shutdown();
    IAllocator& allocator = m_state->allocator;
    m_state->~VulkanDeviceState();
    allocator.Deallocate(m_state, sizeof(Detail::VulkanDeviceState),
                         alignof(Detail::VulkanDeviceState));
    m_state = nullptr;
}

void VulkanDevice::Shutdown() {
    if (m_state != nullptr) {
        m_state->Shutdown();
    }
}

bool VulkanDevice::IsInitialized() const {
    return m_state != nullptr && m_state->device != VK_NULL_HANDLE;
}

u32 VulkanDevice::GraphicsQueueFamilyIndex() const {
    return m_state != nullptr ? m_state->graphicsQueueFamily : 0;
}

const AdapterInfo& VulkanDevice::Adapter() const {
    // A reference has to point at something on a moved-from device too. A function-local
    // static rather than a member, because a moved-from device has no members left: it is
    // const, empty, shared, and never written.
    static const AdapterInfo kNoAdapter{};
    return m_state != nullptr ? m_state->adapter : kNoAdapter;
}

IQueue& VulkanDevice::GraphicsQueue() {
    // A moved-from device has no queue, and a reference has to point at something. `IQueue` is
    // abstract so there is no empty one to make a static of -- except that a `VulkanQueue` with
    // no state attached is exactly that. Every one of its calls already reports
    // `ErrorCode::InvalidArgument` for a null state, because it has to answer that for a
    // *shut-down* device anyway, so the detached queue is a null object rather than a second
    // implementation written for this case. That is why this is not an abort: the caller gets
    // a queue that refuses everything and says so, which is a report it can act on.
    static Detail::VulkanQueue kDetachedQueue;
    return m_state != nullptr ? m_state->queue : kDetachedQueue;
}

Result<TextureHandle> VulkanDevice::CreateTexture(const TextureDescription& description) {
    if (!IsInitialized()) {
        return Err(ErrorCode::InvalidArgument,
                   "IDevice::CreateTexture called on a device that has been shut down or moved "
                   "from");
    }
    if (description.extent.IsEmpty()) {
        return Err(ErrorCode::InvalidArgument,
                   "IDevice::CreateTexture was given an empty extent");
    }
    if (description.format == Format::Unknown) {
        return Err(ErrorCode::InvalidArgument,
                   "IDevice::CreateTexture was given Format::Unknown");
    }
    if (description.usage == TextureUsage::None) {
        // Vulkan rejects a zero usage, and so does this: a texture nothing may be done with is
        // a caller mistake, and catching it here names the mistake rather than the VkResult.
        return Err(ErrorCode::InvalidArgument,
                   "IDevice::CreateTexture was given no usage");
    }

    const u32 slotIndex = m_state->ClaimTextureSlot();
    if (slotIndex == Detail::kNoSlot) {
        MONARC_LOG(Detail::LogCategories::VulkanDevice, Warning,
                   "every one of the {} texture slots this device was configured with is in use",
                   m_state->config.maxTextures);
        return Err(ErrorCode::OutOfMemory, "this device's texture pool is full");
    }
    Detail::TextureSlot& slot = m_state->textures[slotIndex];
    slot.description          = description;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType     = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format    = Detail::ToVulkan(description.format);
    imageInfo.extent    = VkExtent3D{description.extent.width, description.extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = Detail::ToVulkan(description.usage);
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    // UNDEFINED, always. Vulkan permits only UNDEFINED or PREINITIALIZED here, and
    // PREINITIALIZED is for linear images whose memory the host filled before creation -- which
    // is what the staging buffer exists to avoid. So every texture starts in
    // TextureLayout::Undefined, and the caller's first barrier is what moves it.
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (const VkResult result =
            m_state->functions.vkCreateImage(m_state->device, &imageInfo, nullptr, &slot.image);
        result != VK_SUCCESS) {
        slot.image = VK_NULL_HANDLE;
        m_state->ReleaseTextureSlot(slot);
        return m_state->FailVk("vkCreateImage", result);
    }

    VkMemoryRequirements requirements{};
    m_state->functions.vkGetImageMemoryRequirements(m_state->device, slot.image, &requirements);

    // Device-local, always: TextureUsage has no host-visible option, and Device.h says why --
    // a CPU-addressable texture means linear tiling and per-vendor row pitch, which the
    // staging buffer exists to keep out of this.
    Result<VkDeviceMemory> memory =
        m_state->AllocateFor(requirements, MemoryLocation::DeviceLocal);
    if (!memory) {
        const Error error = memory.error();
        m_state->ReleaseTextureSlot(slot);
        return std::unexpected(error);
    }
    slot.memory = *memory;

    if (const VkResult result =
            m_state->functions.vkBindImageMemory(m_state->device, slot.image, slot.memory, 0);
        result != VK_SUCCESS) {
        m_state->ReleaseTextureSlot(slot);
        return m_state->FailVk("vkBindImageMemory", result);
    }

    // **A view only for a texture that can have one, and that is not an optimisation.**
    // Dynamic rendering names a view rather than an image, so an attachment needs one -- but
    // `vkCreateImageView` requires the image to carry at least one view-compatible usage bit,
    // and a transfer-only texture carries none. Creating one unconditionally is a validation
    // error on exactly such a texture, and it is not a hypothetical: it was found here, by the
    // fatal messenger, on the first test that made a `TransferSource`-only texture --
    // `VUID-VkImageViewCreateInfo-image-04441`, "was created with
    // VK_IMAGE_USAGE_2_TRANSFER_SRC_BIT_KHR but requires ... COLOR_ATTACHMENT ...".
    //
    // So a texture with no view is a legitimate texture, and `BeginRendering` is where the
    // consequence lands: an attachment without a view is refused there by name.
    //
    // `ColorAttachment` is the only view-compatible usage `TextureUsage` has today. `Sampled`
    // and `Storage` join this condition when they arrive, and both are view-compatible too.
    if (HasAny(description.usage, TextureUsage::ColorAttachment)) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                       = slot.image;
        viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                      = imageInfo.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel   = 0;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount     = 1;

        // Created eagerly rather than on demand: with one mip level and one layer there is
        // exactly one view a texture can have, so there is nothing to choose and no cache to
        // invalidate. A mip chain turns this into a view per subresource, and it arrives with
        // the first mipped texture.
        if (const VkResult result = m_state->functions.vkCreateImageView(
                m_state->device, &viewInfo, nullptr, &slot.view);
            result != VK_SUCCESS) {
            slot.view = VK_NULL_HANDLE;
            m_state->ReleaseTextureSlot(slot);
            return m_state->FailVk("vkCreateImageView", result);
        }
    }

    // Aggregate initialisation and not `ForTesting`, which is what a test uses to say it is
    // forging a handle no device returned. This is the device.
    return TextureHandle{slotIndex, slot.generation};
}

void VulkanDevice::DestroyTexture(TextureHandle texture) {
    if (!IsInitialized()) {
        return;
    }
    Detail::TextureSlot* slot = m_state->Resolve(texture);
    if (slot == nullptr) {
        // An invalid, unknown or already-destroyed handle is a no-op rather than a report,
        // because "destroy what may or may not still exist" is exactly what a teardown path
        // has. The generation is not bumped here -- the next claim does that, so a slot's
        // generation changes once per occupant.
        return;
    }
    m_state->ReleaseTextureSlot(*slot);
}

Result<BufferHandle> VulkanDevice::CreateBuffer(const BufferDescription& description) {
    if (!IsInitialized()) {
        return Err(ErrorCode::InvalidArgument,
                   "IDevice::CreateBuffer called on a device that has been shut down or moved "
                   "from");
    }
    if (description.size == 0) {
        return Err(ErrorCode::InvalidArgument, "IDevice::CreateBuffer was given a size of zero");
    }
    if (description.usage == BufferUsage::None) {
        return Err(ErrorCode::InvalidArgument, "IDevice::CreateBuffer was given no usage");
    }

    const u32 slotIndex = m_state->ClaimBufferSlot();
    if (slotIndex == Detail::kNoSlot) {
        MONARC_LOG(Detail::LogCategories::VulkanDevice, Warning,
                   "every one of the {} buffer slots this device was configured with is in use",
                   m_state->config.maxBuffers);
        return Err(ErrorCode::OutOfMemory, "this device's buffer pool is full");
    }
    Detail::BufferSlot& slot = m_state->buffers[slotIndex];
    slot.description         = description;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size        = description.size;
    bufferInfo.usage       = Detail::ToVulkan(description.usage);
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (const VkResult result = m_state->functions.vkCreateBuffer(m_state->device, &bufferInfo,
                                                                  nullptr, &slot.buffer);
        result != VK_SUCCESS) {
        slot.buffer = VK_NULL_HANDLE;
        m_state->ReleaseBufferSlot(slot);
        return m_state->FailVk("vkCreateBuffer", result);
    }

    VkMemoryRequirements requirements{};
    m_state->functions.vkGetBufferMemoryRequirements(m_state->device, slot.buffer, &requirements);

    Result<VkDeviceMemory> memory = m_state->AllocateFor(requirements, description.location);
    if (!memory) {
        const Error error = memory.error();
        m_state->ReleaseBufferSlot(slot);
        return std::unexpected(error);
    }
    slot.memory = *memory;

    if (const VkResult result =
            m_state->functions.vkBindBufferMemory(m_state->device, slot.buffer, slot.memory, 0);
        result != VK_SUCCESS) {
        m_state->ReleaseBufferSlot(slot);
        return m_state->FailVk("vkBindBufferMemory", result);
    }

    return BufferHandle{slotIndex, slot.generation};
}

void VulkanDevice::DestroyBuffer(BufferHandle buffer) {
    if (!IsInitialized()) {
        return;
    }
    Detail::BufferSlot* slot = m_state->Resolve(buffer);
    if (slot == nullptr) {
        return;
    }
    m_state->ReleaseBufferSlot(*slot);
}

Result<std::span<const u8>> VulkanDevice::MapBufferForRead(BufferHandle buffer) {
    if (!IsInitialized()) {
        return Err(ErrorCode::InvalidArgument,
                   "IDevice::MapBufferForRead called on a device that has been shut down or "
                   "moved from");
    }
    Detail::BufferSlot* slot = m_state->Resolve(buffer);
    if (slot == nullptr) {
        return Err(ErrorCode::InvalidArgument,
                   "IDevice::MapBufferForRead names a buffer this device does not have, or one "
                   "whose handle is stale");
    }
    if (slot->description.location != MemoryLocation::HostVisible) {
        return Err(ErrorCode::Unsupported,
                   "IDevice::MapBufferForRead was given a device-local buffer, which the CPU "
                   "cannot address");
    }
    if (slot->mapped != nullptr) {
        // Vulkan forbids mapping already-mapped memory. Reporting it beats returning the
        // existing pointer, which would leave two callers each expecting to unmap.
        return Err(ErrorCode::InvalidArgument,
                   "IDevice::MapBufferForRead was given a buffer that is already mapped");
    }

    void* address = nullptr;
    if (const VkResult result = m_state->functions.vkMapMemory(
            m_state->device, slot->memory, 0, VK_WHOLE_SIZE, 0, &address);
        result != VK_SUCCESS) {
        return m_state->FailVk("vkMapMemory", result);
    }
    slot->mapped = address;

    // The buffer's own size and not the memory allocation's, which may be larger: a caller
    // reading past what it asked for would be reading padding the driver chose.
    //
    // No vkInvalidateMappedMemoryRanges, and that is a consequence of asking for coherent
    // memory rather than an omission -- Translate.h's ToVulkan(MemoryLocation) requires
    // HOST_COHERENT alongside HOST_VISIBLE, so device writes are visible once they are
    // available. Availability is the caller's: IQueue::Wait, before reading.
    return std::span<const u8>(static_cast<const u8*>(address),
                               static_cast<usize>(slot->description.size));
}

void VulkanDevice::UnmapBuffer(BufferHandle buffer) {
    if (!IsInitialized()) {
        return;
    }
    Detail::BufferSlot* slot = m_state->Resolve(buffer);
    if (slot == nullptr || slot->mapped == nullptr) {
        return;
    }
    m_state->functions.vkUnmapMemory(m_state->device, slot->memory);
    slot->mapped = nullptr;
}

Result<ICommandList*> VulkanDevice::BeginFrame() {
    if (!IsInitialized()) {
        return Err(ErrorCode::InvalidArgument,
                   "IDevice::BeginFrame called on a device that has been shut down or moved "
                   "from");
    }

    const u32              next = (m_state->frameIndex + 1) % kFramesInFlight;
    Detail::FrameSlot&     slot = m_state->frames[next];

    if (slot.timelineValue != 0) {
        // The wait that makes the pool reset below legal: resetting a command pool whose
        // buffers are still executing is undefined behaviour. Zero means this slot has never
        // been submitted, so there is nothing to wait for.
        if (Status waited =
                m_state->queue.Wait(slot.timelineValue,
                                    Detail::kTimelineWaitTimeoutNanoseconds);
            !waited) {
            return std::unexpected(waited.error());
        }
    }

    if (const VkResult result =
            m_state->functions.vkResetCommandPool(m_state->device, slot.pool, 0);
        result != VK_SUCCESS) {
        return m_state->FailVk("vkResetCommandPool", result);
    }

    m_state->frameIndex = next;
    m_state->lists[next].Reset();
    return &m_state->lists[next];
}

Status VulkanDevice::WaitIdle() {
    if (!IsInitialized()) {
        return Err(ErrorCode::InvalidArgument,
                   "IDevice::WaitIdle called on a device that has been shut down or moved from");
    }
    if (const VkResult result = m_state->functions.vkDeviceWaitIdle(m_state->device);
        result != VK_SUCCESS) {
        return m_state->FailVk("vkDeviceWaitIdle", result);
    }
    return {};
}

}  // namespace Monarc::RHI
