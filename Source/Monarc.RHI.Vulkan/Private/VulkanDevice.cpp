// The logical device, its queue and timeline, its resource pools, and the factory that brings
// every one of them up.
//
// **Command recording is not here.** `ICommandList` is implemented in
// Private/VulkanCommandList.cpp -- the file the A3 plan's listing names -- which also carries
// this module's dynamic-rendering claim and the grep that checks it. The declarations the two
// files share are in Private/VulkanDeviceState.h, which says why the line between them falls
// where it does and why `VulkanQueue` stayed on this side of it.
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
#include <Monarc/RHI/Barrier.h>

#include <ArrayOps.h>
#include <Loader.h>
#include <Translate.h>
#include <VulkanDeviceFactory.h>
#include <VulkanDeviceState.h>

#include <cstdlib>
#include <new>
#include <utility>

namespace Monarc::RHI::Detail {

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
///
/// **Pool slots only.** `FindGraphicsQueueFamily` has `kNoQueueFamily` of its own rather than
/// borrowing this: a queue family index is a different domain, and one constant standing for
/// "not a slot" and "not a family" at once would compare equal across the two by coincidence
/// rather than by meaning.
constexpr u32 kNoSlot = static_cast<u32>(-1);

/// No queue family ever has this index. Returned by `FindGraphicsQueueFamily` when a physical
/// device reports no family that can do graphics.
constexpr u32 kNoQueueFamily = static_cast<u32>(-1);

}  // namespace

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
    // Two independent refusals, and the generation is the one that carries ADR-0002 on its
    // own -- see `TextureSlot::generation` for why it is bumped on release as well as on
    // claim. `live` is kept for the one case the generation cannot answer: a *forged* handle
    // naming a slot no device has ever claimed, whose generation is still zero and which
    // `Handle::ForTesting(index, 0)` therefore matches. No device issues such a handle, so
    // that is not a use-after-destroy -- but the answer must still be "refuse".
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
    // `Resolve(TextureHandle)`'s two refusals, unchanged.
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
            // One of the two bumps a slot's occupant costs; `ReleaseTextureSlot` does the
            // other. This one is what keeps generation zero from ever naming a live slot;
            // that one is what makes the generation sufficient on its own to refuse a
            // destroyed handle. `TextureSlot::generation` has the whole argument.
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

    // The release-side bump. Every handle this slot ever issued is now stale by generation
    // alone, with no wait for the next claim -- see `TextureSlot::generation`. Reached from
    // `DestroyTexture`, from `CreateTexture`'s failure paths, and from `Shutdown`; all three
    // are "this slot becomes free", which is the event the bump is about.
    ++slot.generation;
    slot.live = false;
}

void VulkanDeviceState::ReleaseBufferSlot(BufferSlot& slot) {
    if (slot.mapped != nullptr) {
        // Freeing memory that is still mapped is legal in Vulkan and draws no validation
        // error, so what this branch prevents is not a leak: it is a caller left holding a
        // span into unmapped pages, and a *slot* whose `mapped` still points at the previous
        // occupant's mapping. The second half is the observable one -- with this branch gone,
        // the next buffer to claim the slot is refused by `MapBufferForRead` as "already
        // mapped", which is the assertion "destroying a mapped buffer unmaps it, so its slot
        // can be mapped again" makes.
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

    // `ReleaseTextureSlot`'s release-side bump, for the buffer pool.
    ++slot.generation;
    slot.live = false;
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

    // **The lists are detached with the pools they point into, and that is not tidiness.** A
    // `VulkanCommandList` a caller still holds from `BeginFrame` keeps its `m_recording` flag
    // and its `VkCommandBuffer` across a shutdown, so a recording call made afterwards would
    // dispatch a `vkCmd*` through a freed buffer on a destroyed `VkDevice`. `Shutdown` is
    // documented as safe to call unconditionally and more than once, which puts that ordering
    // in a teardown path rather than only behind a dangling pointer -- and
    // Monarc/RHI/Vulkan/VulkanDevice.h promises every surface this class hands out answers
    // safely on a shut-down device.
    //
    // Measured, both directions. With this loop absent, "a command list outlives its device's
    // shutdown and refuses to record" exits 0xC0000409 and prints nothing -- the process is
    // gone before doctest's output is flushed -- while the rest of the device suite, that case
    // excluded by name, stays green at 35 cases and 338 assertions. So the crash is this
    // ordering and nothing else in the suite reached it.
    for (VulkanCommandList& list : lists) {
        list.Detach();
    }

    if (timeline != VK_NULL_HANDLE) {
        functions.vkDestroySemaphore(device, timeline, nullptr);
        timeline = VK_NULL_HANDLE;
    }

    if (functions.vkDestroyDevice != nullptr) {
        functions.vkDestroyDevice(device, nullptr);
    } else {
        // Reachable only if `Loader::ResolveDeviceDestroyer` also came back null on the
        // factory's failure path -- see `BringUpDevice`, which resolves this one entry back
        // precisely so that this branch is not where a failed bring-up ends. Kept because a
        // null here is still possible in principle and a silent leak is worse than a loud one.
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
    if (!list->IsRecorded()) {
        // **The list straight out of `BeginFrame`, with nothing recorded into it.** Not the
        // same condition as the one above: `BeginFrame` resets the whole pool, which returns
        // the command buffer to Vulkan's initial state, and a list in that state is not
        // recording either. Submitting one is `VUID-vkQueueSubmit2-commandBuffer-03874`
        // ("is unrecorded and contains no commands") and, with the fatal messenger installed,
        // stops the process -- so this is the refusal that keeps a frame loop with an early-out
        // between `BeginFrame` and recording a returned Status instead.
        return Err(ErrorCode::InvalidArgument,
                   "IQueue::Submit was given a command list that has recorded nothing since "
                   "BeginFrame; call Begin and End first");
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

/// Index of the first queue family on `physicalDevice` that can do graphics, or
/// `kNoQueueFamily`.
///
/// First and not best: A3 has one queue and submits everything to it, so there is nothing to
/// optimise between families. A transfer-only or async-compute family is a later phase's
/// choice, and it will want this to become a query over all families rather than a first hit.
[[nodiscard]] u32 FindGraphicsQueueFamily(const Loader& loader, IAllocator& allocator,
                                          VkPhysicalDevice physicalDevice) {
    u32 count = 0;
    loader.Instance().vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);
    if (count == 0) {
        return kNoQueueFamily;
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
    return kNoQueueFamily;
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
        // **The one failure path in this module that leaked a Vulkan object, closed.**
        // `LoadDeviceFunctions` clears the whole table when a Required entry is missing -- that
        // is its promise, and a good one -- so `Shutdown` would find no `vkDestroyDevice` and
        // log that the VkDevice is leaked until the process exits. Resolving that single entry
        // back is enough for the device to be destroyed, and it is the only one needed: nothing
        // else has been created yet, so `Shutdown` has a device and nothing else to unwind.
        //
        // Vanishingly unlikely -- every device entry point Monarc asks for is Vulkan 1.3 core
        // on a device that reported 1.3 -- and reachable only by forcing the failure, which is
        // how it was measured: with `LoadDeviceFunctions` made to fail unconditionally, the
        // leak line disappears from the log and vkDestroyDevice runs.
        state.functions.vkDestroyDevice = loader.ResolveDeviceDestroyer(state.device);
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
    if (queueFamily == kNoQueueFamily) {
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
        // has. Nothing is bumped on this path and nothing should be: the slot did not change
        // hands, so bumping would invalidate handles the *current* occupant issued.
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
        // buffers are still executing is undefined behaviour. Zero means this slot has nothing
        // outstanding, so there is nothing to wait for.
        if (Status waited =
                m_state->queue.Wait(slot.timelineValue,
                                    Detail::kTimelineWaitTimeoutNanoseconds);
            !waited) {
            return std::unexpected(waited.error());
        }
        // Cleared only once the wait has returned, which is the point at which the slot has
        // nothing outstanding -- see `FrameSlot::timelineValue`. A wait that failed leaves the
        // value in place, so the next attempt on this slot waits again rather than resetting a
        // pool whose buffers may still be executing.
        slot.timelineValue = 0;
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
