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

/// No queue family ever has this index. Returned by `FindGraphicsQueueFamily` when a physical
/// device reports no family that can do graphics.
///
/// **Not `kNoSlot`, which VulkanDevice.cpp keeps to itself for pool slots.** A queue family
/// index is a different domain, and one constant standing for "not a slot" and "not a family"
/// at once would compare equal across the two by coincidence rather than by meaning.
inline constexpr u32 kNoQueueFamily = static_cast<u32>(-1);

/// Index of the first queue family on `physicalDevice` that can do graphics, or
/// `kNoQueueFamily`.
///
/// First and not best: A3 has one queue and submits everything to it, so there is nothing to
/// optimise between families. A transfer-only or async-compute family is a later phase's
/// choice, and it will want this to become a query over all families rather than a first hit.
///
/// **Declared here rather than kept in VulkanDevice.cpp's anonymous namespace, because Task 4
/// gave it a second caller in another translation unit.** `VulkanBackend::AdapterCanPresent`
/// asks `vkGetPhysicalDeviceSurfaceSupportKHR` about a family, and the family it has to ask
/// about is the one device creation would actually pick -- so the two must agree by
/// construction rather than by two copies of "the first graphics family". A second copy would
/// answer differently the day this becomes a query over all families.
///
/// `allocator` is used for one scratch array of queue-family properties and nothing survives
/// the call.
[[nodiscard]] u32 FindGraphicsQueueFamily(const Loader& loader, IAllocator& allocator,
                                          VkPhysicalDevice physicalDevice);

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

    /// Whether releasing this slot destroys its `VkImage`.
    ///
    /// **False for exactly one thing: a swapchain image.** `vkGetSwapchainImagesKHR` hands
    /// back images the presentation engine owns; `vkDestroyImage` on one is
    /// `VUID-vkDestroyImage-image-04882`, and destroying the swapchain is what frees them. So
    /// a swapchain registers its images in this pool through `AdoptImage` with this false, and
    /// `ReleaseTextureSlot` skips the image while still destroying the *view* -- the view is
    /// Monarc's, created because dynamic rendering names a view rather than an image.
    ///
    /// `memory` needs no such flag: a swapchain image is not bound to a `VkDeviceMemory` this
    /// device allocated, so the field is null and the existing null test already skips it.
    ///
    /// **Why an adopted image is a pool slot at all**, rather than a swapchain-private array:
    /// it makes an acquired image a `TextureHandle`, so `Barrier`, `BeginRendering` and
    /// `CopyTextureToBuffer` work on it with no new entry point and with ADR-0002's generation
    /// checking intact -- which is what lets the swapchain readback reuse Task 3's copy path
    /// exactly. See `RHI::AcquiredImage`.
    bool ownsImage = true;
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

/// `ICommandList` for one frame slot, and the state machine that decides which of its calls are
/// legal now.
///
/// **The state machine is one `enum class State` and no booleans, which is Task 4's refactor
/// and the whole reason it was deferred to here.** Task 3 wrote the table out and tracked it
/// with `m_recording`, `m_recorded`, `m_submitted` and `m_rendering`; closing the table found
/// three reachable holes, each a named VUID in Debug and **silent undefined behaviour in
/// Release**. Four bools cannot make an unaudited method a compile error. An exhaustive
/// `switch` over `State` can, because `/w44062` is on (CMake/MonarcTargetOptions.cmake) and
/// every switch below is `default`-less: a state added to the enum fails to compile in each
/// method that has to decide about it. That is the same class of guarantee as
/// `TextureBarrier`'s required layout pair -- not testable by assertion, demonstrable by
/// writing the bad code and watching it fail to build.
///
/// **There are five states and not four, and the fifth is not the one the plan predicted.**
/// The A3 plan deferred this refactor on the grounds that "the swapchain adds the fifth state
/// -- a list holding an acquired image is a state the machine does not yet have". It does not.
/// An acquired swapchain image is a `TextureHandle` in the device's own texture pool
/// (`RHI::AcquiredImage`), so a list rendering into one is in exactly the state it is in when
/// rendering into any other texture, and the swapchain adds no list state at all. The fifth
/// state was already here: `Rendering`, which Task 3 tracked as a *sub-state* of `Recording`
/// in a fourth bool. Folding it in is what makes the "+ pass" column of Task 3's table a row
/// of the enum -- and that column is where one of the three holes was.
///
/// | state       | command buffer        | inside `vkCmdBeginRendering` |
/// |-------------|-----------------------|------------------------------|
/// | `Reset`     | initial               | no                           |
/// | `Recording` | recording             | no                           |
/// | `Rendering` | recording             | yes                          |
/// | `Recorded`  | executable            | no                           |
/// | `Submitted` | pending, then invalid | no                           |
///
/// The middle column is the state the `VkCommandBuffer` is in, which is what the validation
/// layer's VUIDs are phrased in terms of. `Recording` and `Rendering` share it, which is
/// exactly why they were one state and a flag: Vulkan does not distinguish them, and Monarc
/// must, because `vkCmdPipelineBarrier2` is legal in one and not the other.
///
/// The transitions, and there are seven:
///
/// - `Attach` and `Reset` -> `Reset`, from any state. `IDevice::BeginFrame` is what calls
///   `Reset`, and it does so *after* waiting on the slot's timeline value and resetting the
///   pool -- which is the step that actually returns the command buffer to `initial`.
/// - `Reset` --`Begin`--> `Recording`.
/// - `Recording` --`BeginRendering`--> `Rendering`.
/// - `Rendering` --`EndRendering`--> `Recording`.
/// - `Recording` --`End`--> `Recorded`. Refused in `Rendering`.
/// - `Recorded` --`IQueue::Submit`--> `Submitted`.
/// - `Submitted` --`IDevice::BeginFrame`--> `Reset`. **The only way out**, and the reason the
///   state outlives the submission: nothing else waits on the timeline, so nothing else may
///   reset the pool.
///
/// **Three combinations of Task 3's four bools were unreachable, and the enum makes that
/// structural rather than argued.** `m_recording && m_recorded` could not occur, nor
/// `m_submitted` without `m_recorded`, nor `m_rendering` outside `m_recording` -- each held
/// because of which writer set what, and each had to be re-checked by hand whenever a writer
/// changed. Five enumerators have no such combinations to reason about: sixteen bool states
/// collapse to the five that were ever real.
///
/// `Detach` is `Reset` plus forgetting the buffer, so a detached list reads as `Reset` with no
/// `VkCommandBuffer`. It therefore needs no state of its own -- and it is genuinely orthogonal
/// rather than a sixth enumerator: `Begin` refuses on the null buffer, and every other call is
/// already refused by `Reset`. Giving it an enumerator would add a row in which every cell
/// repeated `Reset`'s.
///
/// **What each method refuses, checked against every state because the compiler now requires
/// it.** `ok` is a state a call is legal in; `Status` is a returned `ErrorCode::InvalidArgument`;
/// `check` is `MONARC_CHECK` and a plain return, which is all a void function has; `fatal` is
/// `MONARC_CHECK` and `std::abort()`.
///
/// |                       | detached | `Reset`  | `Recording` | `Rendering` | `Recorded` | `Submitted` |
/// |-----------------------|----------|----------|-------------|-------------|------------|-------------|
/// | `Begin`               | `Status` | **ok**   | `Status`    | `Status`    | `Status`   | `Status`    |
/// | `End`                 | `Status` | `Status` | **ok**      | `Status`    | `Status`   | `Status`    |
/// | `Barrier` x3          | `check`  | `check`  | **ok**      | `fatal`     | `check`    | `check`     |
/// | `BeginRendering`      | `Status` | `Status` | **ok**      | `Status`    | `Status`   | `Status`    |
/// | `EndRendering`        | `check`  | `check`  | `check`     | **ok**      | `check`    | `check`     |
/// | `CopyTextureToBuffer` | `Status` | `Status` | **ok**      | `Status`    | `Status`   | `Status`    |
/// | `IQueue::Submit`      | `Status` | `Status` | `Status`    | `Status`    | **ok**     | `Status`    |
///
/// `Submit`'s `Rendering` cell was written "unreachable" while the table was prose, on the
/// grounds that `End` refuses inside a pass and `Submit` refuses a list that is not recorded,
/// so no order of calls presents a mid-pass list to the queue. That reasoning still holds and
/// the cell is now a real `Status` anyway, because the exhaustive switch has to say something
/// and "unreachable" is not something a `switch` can say without a comment nobody re-checks.
/// Refusing costs one case label.
///
/// Three of those cells were holes when the table was first written out, each of them a caller
/// ordering mistake that ended the process at a validation error instead of returning a
/// `Status`: `Begin` in `Recorded`, `Submit` in `Submitted`, and `Barrier` inside a pass. All
/// three were measured on this machine before and after -- see each guard, and Docs/Status.md.
class VulkanCommandList final : public ICommandList {
public:
    /// The five states of the machine above, as one value.
    ///
    /// Nested, so the name is `VulkanCommandList::State` at every use and there is no
    /// `State` at namespace scope for a second state machine to collide with. Public,
    /// because `VulkanQueue::Submit` switches over it -- and that method is the one this
    /// enum exists for: it is where two of Task 3's three holes were, and an exhaustive
    /// switch there is what makes a sixth state impossible to add without auditing it.
    ///
    /// `u8` rather than the default `int`: five enumerators, and this is a per-frame member
    /// of a per-device array. The width is not what matters -- being explicit about it is,
    /// because a fixed underlying type is what makes a value outside the enumerator set
    /// representable, and every trailing `return`/`MONARC_CHECK` after a `default`-less
    /// switch below exists for exactly that value.
    enum class State : u8 {
        /// Nothing recorded; the command buffer is in Vulkan's initial state. What
        /// `Attach`, `Reset` and `Detach` leave behind.
        Reset = 0,

        /// `Begin` has run and `End` has not. Commands may be recorded.
        Recording,

        /// Recording, and inside the rendering instance `BeginRendering` began. Every
        /// `vkCmd*` Monarc records is legal here **except** `vkCmdPipelineBarrier2`, which
        /// is why this is a state and not a flag.
        Rendering,

        /// `End` has run: the command buffer is executable and a queue may take it.
        Recorded,

        /// A queue has taken this recording. Still recorded -- the recording in the buffer is
        /// the one that was submitted -- but no longer submittable, because `Begin` recorded
        /// it with `VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT` and the buffer therefore
        /// becomes *invalid* rather than executable once the work finishes. A `WaitIdle`
        /// does not make a second submission legal; only `IDevice::BeginFrame` does.
        Submitted,
    };

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

    /// Which state this list is in.
    ///
    /// **This replaced four boolean accessors -- `IsRecording`, `IsRendering`, `IsRecorded`
    /// and `IsSubmitted` -- and their loss is the point.** Each answered one question about
    /// the state, so a caller deciding what to do had to ask several and get the combination
    /// right; `VulkanQueue::Submit` asked three and, until Task 3's review, drew the wrong
    /// conclusion from two of them. One value asked once cannot be combined wrongly, and a
    /// `switch` over it is checked for completeness by the compiler where a chain of `if`s is
    /// not.
    [[nodiscard]] State CurrentState() const { return m_state; }

    /// Records that `IQueue::Submit` has taken this recording. Called by `VulkanQueue::Submit`
    /// once `vkQueueSubmit2` has succeeded, beside the stamp it puts on the frame slot.
    ///
    /// Unconditional rather than guarded on the current state, because the guard is the
    /// caller's: `Submit` refuses every state but `Recorded` before it reaches the API, and a
    /// second check here would be a second place for that rule to live.
    void MarkSubmitted() { m_state = State::Submitted; }

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
    /// Refuses a barrier that cannot be recorded now, reporting through the assertion handler
    /// and -- in the one state where there is nothing legal to record instead -- ending the
    /// process. Returns true when the caller may go on and record.
    ///
    /// **One function for the whole `Barrier` row of the table, because all three overloads
    /// have that row and no `Status` to refuse through.** Void rather than `Status` for the
    /// three is deliberate and mirrors the API: Vulkan's own `vkCmd*` functions return
    /// nothing, because a command buffer defers every error it can to `vkEndCommandBuffer` --
    /// which is what `End()` above surfaces. Recording on a list that is not recording is not
    /// a deferred error, it is a caller bug, and the house pattern for one is a check.
    ///
    /// This replaced `CanRecord` plus an `m_rendering` test written out three times, and the
    /// exhaustive switch inside it is why: with the states enumerated, "which states may
    /// record a barrier" is one question with one answer, and the compiler checks that the
    /// answer covers them all.
    [[nodiscard]] bool CanRecordBarrier(const char* operation) const;

    /// Reports `operation` and ends the process. What `CanRecordBarrier` calls in `Rendering`,
    /// which is the one cell of the table above where a void function has no legal call to
    /// make and no `Status` to refuse through. See the definition for why it is fatal rather
    /// than a return.
    [[noreturn]] static void FailInsideRenderingPass(const char* operation);

    /// The device this list records into. **Named `m_deviceState` and not `m_state`, which is
    /// what it was called through Task 3**: with a `State` member beside it, `m_state` would
    /// have been the device in one line and the machine's state in the next.
    VulkanDeviceState* m_deviceState = nullptr;
    VkCommandBuffer    m_buffer      = VK_NULL_HANDLE;
    u32                m_frameIndex  = 0;
    State              m_state       = State::Reset;
};

/// The enumerator's own spelling, for logs and test failures. Never nullptr; a value outside
/// the enumerator set gets a name of its own, for the reason `RHI::ToString(Format)` in
/// Monarc/RHI/Types.h gives.
///
/// A free function in `Detail` rather than a static member, so it joins the `ToString`
/// overload set `Translate.h` already has -- a static member of that name would *hide* those
/// overloads inside every method of this class, and the first `ToString(result)` in one would
/// stop compiling for a reason nowhere near its cause.
[[nodiscard]] const char* ToString(VulkanCommandList::State state);

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

    /// Whether `VK_KHR_swapchain` was enabled on this device, and its five entry points
    /// therefore resolved.
    ///
    /// **Not a `Capabilities` field and not a tier requirement, deliberately.**
    /// `Capabilities`' own rule is that every field in it is an input to the tier ladder, and
    /// making presentation a `Baseline` requirement would declare a compute-only or headless
    /// device unable to run Monarc's renderer -- which is a claim A3 has no business making,
    /// with `Monarc.Host.Headless` on the roadmap and excluded from this phase by name. So the
    /// extension is enabled where it exists, recorded here, and refused by
    /// `VulkanSwapchainFactory::Create` where it does not: the only call that actually needs
    /// it is the only one that reports about it.
    ///
    /// True on both local adapters, measured.
    bool swapchainEnabled = false;

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

    /// The `VulkanCommandList` `list` names, if it is one of this device's and is in a state a
    /// queue may take. Otherwise a `Status` saying which of the two it failed.
    ///
    /// **Shared by the two submitting calls, which is why it is here rather than inside
    /// `VulkanQueue::Submit`.** `IQueue::Submit` and `ISwapchain::SubmitForPresent` refuse the
    /// same four caller mistakes -- a foreign list, a list still recording, one that recorded
    /// nothing, one already submitted -- and they are defined in different translation units,
    /// so the alternative was two copies of a guard whose whole value is being the same in
    /// both places. Task 3's review found the third of those four mistakes by writing the
    /// state table out; a second copy of the table's `Submit` row is exactly what that review
    /// was about.
    [[nodiscard]] Result<VulkanCommandList*> ValidateForSubmit(const char*   operation,
                                                               ICommandList& list);

    /// Submits `list`'s recording on the graphics queue and returns the timeline value it will
    /// signal, stamping the frame slot and marking the list submitted.
    ///
    /// `waitBinary` and `signalBinary` are presentation's two semaphores and are
    /// `VK_NULL_HANDLE` for an ordinary submission. Both are per-submission rather than stored
    /// because they belong to a swapchain, and a device may have none or several.
    ///
    /// `list` must already have been through `ValidateForSubmit`: this function records the
    /// submission and does not police the caller's ordering.
    [[nodiscard]] Result<u64> SubmitList(VulkanCommandList& list, VkSemaphore waitBinary,
                                         VkSemaphore signalBinary);

    [[nodiscard]] u32 ClaimTextureSlot();
    [[nodiscard]] u32 ClaimBufferSlot();

    /// Registers an image this device did not create in the texture pool, with a fresh view,
    /// and returns a handle naming its slot.
    ///
    /// For swapchain images and nothing else -- see `TextureSlot::ownsImage`, which this sets
    /// false. `description` is what `BeginRendering` and `CopyTextureToBuffer` read to decide
    /// whether the image may be an attachment or a copy source, so it must be the usage the
    /// *swapchain* was created with rather than a guess.
    ///
    /// Fails with `ErrorCode::OutOfMemory` when the texture pool is full, and with whatever
    /// `vkCreateImageView` said. Nothing is left claimed on a failure.
    [[nodiscard]] Result<TextureHandle> AdoptImage(VkImage                   image,
                                                   const TextureDescription& description);

    [[nodiscard]] Result<VkDeviceMemory> AllocateFor(const VkMemoryRequirements& requirements,
                                                     MemoryLocation              location);

    void ReleaseTextureSlot(TextureSlot& slot);
    void ReleaseBufferSlot(BufferSlot& slot);

    void Shutdown();
};

}  // namespace Monarc::RHI::Detail
