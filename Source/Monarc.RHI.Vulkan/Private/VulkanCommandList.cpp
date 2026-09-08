// Command recording: `ICommandList` as Vulkan 1.3 implements it -- `vkBeginCommandBuffer`,
// `vkCmdPipelineBarrier2`, dynamic rendering, and one copy.
//
// **The file the A3 plan named, and what it holds is the whole of one interface.** Nothing here
// creates, destroys or owns anything: every function is a call recorded into a
// `VkCommandBuffer` a frame slot already allocated, guarded by the recording state this class
// tracks and by `VulkanDeviceState::Resolve` for the handles a caller passes in. The device
// that owns the pools, the queue that submits what is recorded here, and the factory that
// brings both up are in VulkanDevice.cpp; the declarations both files share are in
// Private/VulkanDeviceState.h, which says why the split falls where it does.
//
// **Every state test in this file is one `default`-less switch over `VulkanCommandList::State`,
// and that is Task 4's refactor.** Task 3 tracked the machine in four bools and closed three
// holes one at a time; four bools cannot make an unaudited method a compile error, and with
// `/w44062` on an exhaustive switch can. `VulkanCommandList`'s class comment in
// Private/VulkanDeviceState.h has the states, the transitions, and the cell-by-cell account of
// what each method refuses where -- this file is that table, executable.
//
// The switches carry a case per state even where several share an answer, rather than folding
// the sharers into a `default`. `default` would compile forever: C4062 fires only on a switch
// that has none, which is the whole mechanism (see CMake/MonarcTargetOptions.cmake, which
// explains why /w44061 is deliberately *not* on).
//
// **No VkRenderPass and no VkFramebuffer, here or anywhere else in this module.** Rendering
// begins with vkCmdBeginRendering against image views named at record time -- Vulkan 1.3
// dynamic rendering, which is the model D3D12 and Metal already have. Nothing is created ahead
// of a frame, nothing has to be invalidated when a texture is recreated, and there are no
// pipeline/pass compatibility rules to encode.
//
// The claim is checkable rather than asserted: `grep -rniE "renderpass|framebuffer|render pass"
// Source/` matches four lines, all of them comments -- two here and two in Monarc/RHI/Device.h
// -- and no code. Under VK_NO_PROTOTYPES a call could only reach one of those entry points
// through Private/Loader.h's tables, which name none of them.

#include <VulkanDeviceState.h>

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Log.h>
#include <Monarc/RHI/Barrier.h>
#include <Monarc/RHI/Types.h>

#include <Translate.h>

#include <cstdlib>

namespace Monarc::RHI::Detail {

const char* ToString(VulkanCommandList::State state) {
    using State = VulkanCommandList::State;
    switch (state) {
        case State::Reset:     return "Reset";
        case State::Recording: return "Recording";
        case State::Rendering: return "Rendering";
        case State::Recorded:  return "Recorded";
        case State::Submitted: return "Submitted";
    }
    // The trailing return every `default`-less switch over a fixed-underlying-type enum needs.
    // Unlike the switches in the methods below, this one takes its value as a *parameter*, so
    // it genuinely can be handed something outside the set -- `RHI::ToString(Format)` in
    // Monarc/RHI/Types.h states the same distinction.
    return "<invalid VulkanCommandList::State>";
}

void VulkanCommandList::Attach(VulkanDeviceState* state, u32 frameIndex) {
    m_deviceState = state;
    m_frameIndex  = frameIndex;
    m_buffer      = state->frames[frameIndex].commandBuffer;
    m_state       = State::Reset;
}

void VulkanCommandList::Reset() {
    // **One assignment where Task 3 had four, and the four had to agree.** `BeginFrame` is the
    // caller that matters: it waits on the slot's timeline value and resets the pool, which is
    // what returns the command buffer from pending -- or, once execution finished, from
    // invalid -- to initial. Nothing else in this class or in `VulkanDevice` puts a list back
    // to `Reset`, which is what makes `BeginFrame` the only thing that makes a list recordable
    // and submittable again.
    //
    // A list left mid-recording, or mid-*rendering*, by a caller that never called `End` does
    // not carry that state into the next frame: the assignment covers every state at once,
    // where four separate clears were four chances to forget one. (Task 3 measured exactly
    // that: `Reset` failing to clear the submitted flag turned two `REQUIRE`s red, and failing
    // to clear the recorded flag turned one.)
    m_state = State::Reset;
}

void VulkanCommandList::Detach() {
    Reset();
    // The command buffer is gone with its pool, and this is what `Begin` sees. Detachment is
    // orthogonal to the five states rather than a sixth: `Begin` refuses on the null buffer,
    // and every other call is already refused by `Reset`.
    m_buffer = VK_NULL_HANDLE;
}

bool VulkanCommandList::CanRecordBarrier(const char* operation) const {
    switch (m_state) {
        case State::Recording:
            return true;

        case State::Reset:
        case State::Recorded:
        case State::Submitted:
            // The state name goes to the log and the literal to the check, which is the house
            // split: `MONARC_CHECK`'s message is a string literal by rule, so anything
            // composed -- here, *which* of the four illegal states the list was in -- has to
            // be a log line beside it. That is the same reason the stale-handle refusals below
            // call `Describe`.
            MONARC_LOG(LogCategories::VulkanDevice, Error,
                       "a barrier cannot be recorded on a command list in state {}",
                       ToString(m_state));
            MONARC_CHECK(false, operation);
            return false;

        case State::Rendering:
            // Ends the process. See `FailInsideRenderingPass` for why this cell is fatal where
            // the four above return.
            FailInsideRenderingPass(operation);
    }
    // Unreachable, and for a different reason than `ToString`'s trailing return above:
    // `m_state` is a private member and every writer in this class assigns an enumerator, so
    // there is no value to arrive here. The compiler still needs a return past a
    // `default`-less switch, and refusing is the conservative answer if one ever did.
    return false;
}

void VulkanCommandList::FailInsideRenderingPass(const char* operation) {
    // **`CanRecordBarrier`'s report and `Barrier`'s abort, in one place because the three
    // overloads need both and neither has a `Status` to carry it.** The literal is the
    // caller's, for the house rule's reason; the break and the `std::abort()` are
    // `OnResourceExhausted`'s shape in Monarc.Jobs and the stale-handle guard's shape below.
    //
    // **Fatal rather than a plain return, and this is the one guard in this file where the
    // choice is forced.** `vkCmdPipelineBarrier2` may not be called inside a rendering
    // instance begun by `vkCmdBeginRendering` at all -- not with different flags, not with a
    // different barrier, not with an empty one -- so there is no legal call to record and
    // nothing this function could do instead. `MONARC_CHECK` alters no control flow, so a
    // `return` here would *skip the barrier* under any handler that declines to break, and
    // Monarc/RHI/Device.h argues at length why a dropped barrier is a synchronisation hole
    // rather than a refused operation. The other half of that argument is what makes
    // `CanRecordBarrier`'s plain return right in the four states where this is not: a list
    // that is not recording has no open command buffer, so skipping is the only thing
    // available there.
    MONARC_CHECK(false, operation);
    MONARC_DEBUG_BREAK();
    std::abort();
}

Status VulkanCommandList::Begin() {
    if (m_deviceState == nullptr || m_buffer == VK_NULL_HANDLE) {
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::Begin called on a list that belongs to no device");
    }

    switch (m_state) {
        case State::Reset:
            break;

        case State::Recording:
            return Err(ErrorCode::InvalidArgument,
                       "ICommandList::Begin called on a list that is already recording");

        case State::Rendering:
            // A distinct message from the one above, because a caller in this state has made
            // two mistakes and the second is the one worth naming: the pass is still open, so
            // even a correct `End` would refuse until `EndRendering` has run.
            return Err(ErrorCode::InvalidArgument,
                       "ICommandList::Begin called on a list that is already recording, inside "
                       "a rendering pass; call EndRendering and End first");

        case State::Recorded:
        case State::Submitted:
            // **The other half of the same mistake, and it is a distinct condition rather than
            // a spelling of "already recording".** A list that `End` closed is in Vulkan's
            // *executable* state, and `vkBeginCommandBuffer` on one of those is an implicit
            // reset -- `VUID-vkBeginCommandBuffer-commandBuffer-00050`, which needs
            // `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` on the pool. `BringUpDevice`
            // deliberately does not set it: `BeginFrame` resets the whole pool and that is the
            // only reset this design performs. Once the list has also been submitted the
            // buffer is *pending* instead and the same call is
            // `VUID-vkBeginCommandBuffer-commandBuffer-00049`.
            //
            // The two share a case label because they share the answer, and Task 3's version
            // shared a *guard* -- `m_recorded` outliving `Submit` is what made one `if` cover
            // both. Two labels is the same coverage with the states named.
            //
            // Measured, on this machine, before the guard: `BeginFrame -> Begin -> End ->
            // Begin` stopped at 00050 and exit 3221226505 with doctest reporting the case
            // CRASHED, and the same four calls with a `Submit` before the second `Begin`
            // stopped at 00049. In a build with no validation layer both exited zero and said
            // nothing, which is what makes this a returned `Status` rather than a comment
            // saying the layer catches it.
            //
            // **Reachable from the frame loop Task 4 builds**, which is why Task 3 added it: a
            // loop that records, submits and comes round again without a fresh `BeginFrame`
            // makes exactly this call.
            return Err(ErrorCode::InvalidArgument,
                       "ICommandList::Begin called on a list that has already been recorded; "
                       "call IDevice::BeginFrame for a fresh one");
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    // ONE_TIME_SUBMIT, because that is what this is: `BeginFrame` resets the whole pool before
    // handing the list back, so a buffer is never submitted twice. Telling the driver so is
    // free and lets it discard the recording after submission.
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (const VkResult result =
            m_deviceState->functions.vkBeginCommandBuffer(m_buffer, &beginInfo);
        result != VK_SUCCESS) {
        return m_deviceState->FailVk("vkBeginCommandBuffer", result);
    }
    m_state = State::Recording;
    return {};
}

Status VulkanCommandList::End() {
    switch (m_state) {
        case State::Recording:
            break;

        case State::Rendering:
            // Ending a command buffer inside a rendering pass is a validation error, so
            // catching it here turns a process-stopping abort into a returned Status. The list
            // stays in `Rendering`, because it is: nothing has been ended.
            return Err(ErrorCode::InvalidArgument,
                       "ICommandList::End called inside a rendering pass; call EndRendering "
                       "first");

        case State::Reset:
        case State::Recorded:
        case State::Submitted:
            return Err(ErrorCode::InvalidArgument,
                       "ICommandList::End called on a list that is not recording");
    }

    if (const VkResult result = m_deviceState->functions.vkEndCommandBuffer(m_buffer);
        result != VK_SUCCESS) {
        return m_deviceState->FailVk("vkEndCommandBuffer", result);
    }
    // Moved only after vkEndCommandBuffer succeeded, so a failed End leaves the list recording
    // rather than claiming a recording the driver rejected.
    m_state = State::Recorded;
    return {};
}

void VulkanCommandList::Barrier(const GlobalBarrier& barrier) {
    if (!CanRecordBarrier(
            "ICommandList::Barrier(GlobalBarrier) on a list that cannot record one")) {
        return;
    }

    // Recorded even when nothing changes. `GlobalBarrier{}` is a value a caller can mean --
    // Barrier.h says why -- and dropping it here would be this function deciding on the
    // caller's behalf with less information than the caller had.
    //
    // It is also what tripped the rendering-pass VUID in Task 3's measurement, which is the
    // useful half: the rule is about the call and not about the barrier's contents, so there is
    // no no-op barrier that slips through `CanRecordBarrier`.
    const VkMemoryBarrier2 translated = ToVulkan(barrier);

    VkDependencyInfo dependency{};
    dependency.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers    = &translated;
    m_deviceState->functions.vkCmdPipelineBarrier2(m_buffer, &dependency);
}

void VulkanCommandList::Barrier(const BufferBarrier& barrier) {
    if (!CanRecordBarrier(
            "ICommandList::Barrier(BufferBarrier) on a list that cannot record one")) {
        return;
    }
    const BufferSlot* slot = m_deviceState->Resolve(barrier.buffer);
    if (slot == nullptr) {
        // **The composed detail beside the literal message, and this is the call in the module
        // that needs it most.** `Barrier` returns void -- see `ICommandList::Barrier` for why
        // -- so a `MONARC_CHECK` message is the only channel this refusal has, and the house
        // rule keeps that message a literal. Which barrier the caller got wrong therefore has
        // to come from here. The scopes are what identify it: a frame records several buffer
        // barriers and "a buffer handle was stale" does not say which.
        //
        // `Describe` and not a format string open-coded here, and the reason is the abort
        // below: this line cannot be observed by any test, because nothing survives the call
        // to read it. Composing the text in Monarc.RHI -- where it needs no device and no
        // Vulkan -- is what puts that composition under a test that runs in CI. See the note
        // at the foot of Monarc/RHI/Barrier.h.
        const BarrierDescription described = Describe(barrier);
        MONARC_LOG(LogCategories::VulkanDevice, Error,
                   "this barrier names a resource this device does not have, or one whose "
                   "handle is stale -- {}",
                   described.View());
        MONARC_CHECK(false,
                     "ICommandList::Barrier(BufferBarrier) names a buffer this device does "
                     "not have, or one whose handle is stale");
        // Unconditional, and for the reason the whole house rule exists: MONARC_CHECK reports
        // and optionally breaks, never altering control flow, so under a handler that declines
        // to break -- Shipping, or any test harness -- a plain `return` here *skips the
        // barrier* and records the rest of the frame without it. A dropped barrier is not a
        // refused operation; it is a synchronisation hole whose symptom is wrong pixels or a
        // GPU hang on some driver, days later, with nothing pointing back here.
        //
        // This is JobSystem::Wait's guard again (Docs/Runtime/Threading.md) and the debug
        // messenger's a few files over: where the return type cannot carry a refusal, the
        // process stops rather than continuing as though the call had not happened.
        //
        // Measured both ways rather than reasoned about, and Docs/Status.md records the run:
        // with a declining handler installed and a stale handle passed in, this ends the
        // process (0x80000003, the debug break) and the caller's next line never runs; with
        // the `return` this replaced, the same program printed `BARRIER RETURNED` and exited
        // zero.
        MONARC_DEBUG_BREAK();
        std::abort();
    }

    const VkBufferMemoryBarrier2 translated = ToVulkan(barrier, slot->buffer);

    VkDependencyInfo dependency{};
    dependency.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers    = &translated;
    m_deviceState->functions.vkCmdPipelineBarrier2(m_buffer, &dependency);
}

void VulkanCommandList::Barrier(const TextureBarrier& barrier) {
    if (!CanRecordBarrier(
            "ICommandList::Barrier(TextureBarrier) on a list that cannot record one")) {
        return;
    }
    const TextureSlot* slot = m_deviceState->Resolve(barrier.Texture());
    if (slot == nullptr) {
        // `Barrier(BufferBarrier)`'s reasoning, and `Describe`'s texture overload puts the
        // layout pair in front for the reason Barrier.h gives: a layout is one value and never
        // a mask, so those two names are always real spellings.
        const BarrierDescription described = Describe(barrier);
        MONARC_LOG(LogCategories::VulkanDevice, Error,
                   "this barrier names a resource this device does not have, or one whose "
                   "handle is stale -- {}",
                   described.View());
        MONARC_CHECK(false,
                     "ICommandList::Barrier(TextureBarrier) names a texture this device does "
                     "not have, or one whose handle is stale");
        // `Barrier(BufferBarrier)`'s reasoning, and a layout transition dropped silently is
        // the worse half of it: the image stays in whatever layout it was in, and the next
        // command reads it as though the transition had happened. Measured the same way, and
        // with the same two outcomes.
        //
        // **Task 4 gave this refusal a second population to protect: swapchain images.** They
        // are pool slots like any other (`VulkanDeviceState::AdoptImage`), and
        // `ISwapchain::Recreate` releases every one of them -- so a handle held across a
        // resize is stale, and the frame that barriered it would otherwise be transitioning an
        // image the presentation engine has already destroyed.
        MONARC_DEBUG_BREAK();
        std::abort();
    }

    const VkImageMemoryBarrier2 translated = ToVulkan(barrier, slot->image);

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &translated;
    m_deviceState->functions.vkCmdPipelineBarrier2(m_buffer, &dependency);
}

Status VulkanCommandList::BeginRendering(const RenderingDescription& description) {
    switch (m_state) {
        case State::Recording:
            break;

        case State::Rendering:
            return Err(ErrorCode::InvalidArgument,
                       "ICommandList::BeginRendering called inside a rendering pass");

        case State::Reset:
        case State::Recorded:
        case State::Submitted:
            return Err(ErrorCode::InvalidArgument,
                       "ICommandList::BeginRendering called on a list that is not recording");
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
        const TextureSlot*     slot       = m_deviceState->Resolve(attachment.texture);
        if (slot == nullptr) {
            // The generation check is the whole point of ADR-0002: a handle naming a slot that
            // has been recycled resolves to a failure rather than to whatever now occupies it.
            // A swapchain image handle held across `ISwapchain::Recreate` is exactly such a
            // handle, and this is where a frame loop that kept one is told so.
            return Err(ErrorCode::InvalidArgument,
                       "ICommandList::BeginRendering names a texture this device does not "
                       "have, or one whose handle is stale");
        }
        if (!HasAny(slot->description.usage, TextureUsage::ColorAttachment)) {
            // A texture created without `TextureUsage::ColorAttachment` has no image view, and
            // dynamic rendering needs one -- see `CreateTexture`, where the eager version of
            // that view was a validation error on a transfer-only texture. Refusing here names
            // the missing usage; passing VK_NULL_HANDLE through would be
            // VUID-VkRenderingInfo-colorAttachmentCount-06087 and would stop the process.
            //
            // **The usage and not `slot->view == VK_NULL_HANDLE`, which is equivalent only for
            // as long as `ColorAttachment` is the one view-compatible usage `TextureUsage`
            // has.** `CreateTexture` says that changes: `Sampled` and `Storage` are
            // view-compatible too and join the condition that makes a view, at which point a
            // `Sampled`-only texture *has* a view, passes a null check, and becomes the
            // validation error this refusal exists to prevent. The message names the usage, so
            // the test is the usage.
            //
            // Measured: the two forms are observationally identical today -- with the null-view
            // test back in, the device suite is green -- so this is a change no test can
            // distinguish and is stated as such rather than claimed to be caught. What *is*
            // caught, by "a copy into a buffer too small to hold the texture is refused", is
            // either form being removed altogether.
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

    m_deviceState->functions.vkCmdBeginRendering(m_buffer, &renderingInfo);
    m_state = State::Rendering;
    return {};
}

void VulkanCommandList::EndRendering() {
    switch (m_state) {
        case State::Rendering:
            break;

        case State::Recording:
            MONARC_CHECK(false, "ICommandList::EndRendering with no rendering pass begun");
            return;

        case State::Reset:
        case State::Recorded:
        case State::Submitted:
            // A distinct message from the one above: "no pass begun" would be misleading for a
            // list that is not recording at all, where the caller's mistake is one step
            // earlier.
            MONARC_CHECK(false, "ICommandList::EndRendering on a list that is not recording");
            return;
    }

    m_deviceState->functions.vkCmdEndRendering(m_buffer);
    m_state = State::Recording;
}

Status VulkanCommandList::CopyTextureToBuffer(TextureHandle source, BufferHandle destination) {
    switch (m_state) {
        case State::Recording:
            break;

        case State::Rendering:
            // A copy inside a rendering pass is a validation error. Returning is better than
            // recording it and having the messenger stop the process.
            return Err(ErrorCode::InvalidArgument,
                       "ICommandList::CopyTextureToBuffer called inside a rendering pass");

        case State::Reset:
        case State::Recorded:
        case State::Submitted:
            return Err(ErrorCode::InvalidArgument,
                       "ICommandList::CopyTextureToBuffer called on a list that is not "
                       "recording");
    }

    const TextureSlot* texture = m_deviceState->Resolve(source);
    if (texture == nullptr) {
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::CopyTextureToBuffer's source texture is not one this device "
                   "has, or its handle is stale");
    }
    const BufferSlot* buffer = m_deviceState->Resolve(destination);
    if (buffer == nullptr) {
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::CopyTextureToBuffer's destination buffer is not one this "
                   "device has, or its handle is stale");
    }

    // **Both usages, refused by name, for `BeginRendering`'s reason one paragraph up in this
    // file.** A copy out of an image with no `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` is
    // `VUID-VkCopyImageToBufferInfo2-srcImage-00186` and a copy into a buffer with no
    // `VK_BUFFER_USAGE_TRANSFER_DST_BIT` is `VUID-...-dstBuffer-00191`; both stop the process
    // with the fatal messenger installed, and both are the same class of caller mistake as an
    // attachment created without `TextureUsage::ColorAttachment`, which is already a returned
    // `Status`. The descriptions are on the slots, so each is one `HasAny`.
    //
    // **A swapchain image reaches the first of these**, and that is why the swapchain readback
    // has to ask for the usage rather than assume it: a surface that does not offer
    // `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` produces images whose `TextureDescription` lacks
    // `TextureUsage::TransferSource`, and a copy from one is refused here by name instead of
    // becoming that VUID.
    if (!HasAny(texture->description.usage, TextureUsage::TransferSource)) {
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::CopyTextureToBuffer's source texture was created without "
                   "TextureUsage::TransferSource, so it cannot be copied out of");
    }
    if (!HasAny(buffer->description.usage, BufferUsage::TransferDestination)) {
        return Err(ErrorCode::InvalidArgument,
                   "ICommandList::CopyTextureToBuffer's destination buffer was created without "
                   "BufferUsage::TransferDestination, so it cannot be copied into");
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

    m_deviceState->functions.vkCmdCopyImageToBuffer2(m_buffer, &copyInfo);
    return {};
}

}  // namespace Monarc::RHI::Detail
