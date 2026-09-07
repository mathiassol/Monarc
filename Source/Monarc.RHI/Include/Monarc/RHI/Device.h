#pragma once

#include <Monarc/Core/Error.h>
#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Adapter.h>
#include <Monarc/RHI/Barrier.h>
#include <Monarc/RHI/Handles.h>
#include <Monarc/RHI/Types.h>

#include <span>

namespace Monarc::RHI {

/// How many frames Monarc keeps in flight, and therefore how many command pools a device
/// owns.
///
/// Two: enough to overlap CPU and GPU, few enough that latency stays comprehensible while
/// there is no frame pacing work to reason about. Frame pacing is deferred explicitly --
/// Docs/Rendering/RHI.md, "things that belong here but arrive later".
inline constexpr u32 kFramesInFlight = 2;

/// What a texture will be used for. A device needs this at creation because both APIs the RHI
/// is shaped from bake usage into the resource.
///
/// **Short on purpose, and by a different rule than `Barrier.h`'s stage list.** The plan asks
/// that the *barrier model* arrive whole, because its unused half costs one switch row and is
/// covered by pure-function tests. A usage bit is not that: it is a claim about a resource a
/// backend must create differently, so `Sampled` and `Storage` arrive with the first shader
/// that binds one -- the same rule `Format` in Types.h states for itself.
/// **Two usages, and `TransferDestination` is absent on purpose.** Nothing uploads *to* a
/// texture yet -- the readback's texture is `ColorAttachment | TransferSource` and its
/// destination is a buffer -- so an image transfer-destination bit would be an enumerator with
/// no caller, which is the shape this rule exists to keep out. It arrives with the first thing
/// that uploads, alongside the writable mapping that feeds it.
enum class TextureUsage : u32 {
    None            = 0,
    ColorAttachment = 1u << 0,
    TransferSource  = 1u << 1,
};

/// What a buffer will be used for. `TextureUsage`'s membership rule applies: vertex, index,
/// uniform and storage usages arrive with the first draw that needs one.
enum class BufferUsage : u32 {
    None                = 0,
    TransferSource      = 1u << 0,
    TransferDestination = 1u << 1,
};

namespace Detail {

template <>
inline constexpr bool kIsFlagSet<TextureUsage> = true;

template <>
inline constexpr bool kIsFlagSet<BufferUsage> = true;

}  // namespace Detail

/// Where a buffer's memory lives, and therefore whether the CPU can address it.
///
/// Not a set: a buffer's memory is in one place. Textures do not carry this -- every texture
/// A3 creates is device-local, and a host-visible texture means dealing with linear tiling and
/// per-vendor row pitch, which the staging buffer exists to avoid.
enum class MemoryLocation : u32 {
    /// Fast for the GPU, not addressable by the CPU.
    DeviceLocal = 0,

    /// Addressable by the CPU through `IDevice::MapBufferForRead`. Monarc asks for coherent
    /// host memory, so a mapped read needs no explicit cache invalidation -- every Vulkan
    /// implementation is required to expose at least one host-visible, host-coherent memory
    /// type.
    HostVisible,
};

/// What happens to an attachment's existing contents when rendering begins.
enum class LoadOp : u32 {
    /// Keep what is there.
    Load = 0,

    /// Overwrite every pixel with the attachment's clear value. **This is how Monarc clears.**
    /// There is no separate clear command in this interface, deliberately: the swapchain clear
    /// in Task 4 and the render graph in A4 both clear through a load-op, so a second path
    /// would be a code path nothing ships.
    Clear,

    /// The previous contents are not needed and are not preserved. Cheaper than `Load` on
    /// tiled hardware; identical to it in observable behaviour only if nothing reads the
    /// undefined result.
    DontCare,
};

/// What happens to an attachment's contents when rendering ends.
enum class StoreOp : u32 {
    Store = 0,
    DontCare,
};

/// A clear value for a colour attachment, in the attachment's own channel order.
///
/// Floats even for a UNORM target: that is how both APIs take a clear value, and the
/// conversion is the implementation's. `{64.0F / 255.0F, 128.0F / 255.0F, 192.0F / 255.0F,
/// 1.0F}` is what Task 3's readback test writes and reads back as exactly `(64, 128, 192,
/// 255)` -- see TestsDevice/TestVulkanDevice.cpp in Monarc.RHI.Vulkan.
///
/// Integer and depth/stencil clear values are absent for `Format`'s reason in Types.h: A3 has
/// only UNORM colour formats, and a union of clear kinds with one arm reachable would be a
/// shape a later phase has to justify rather than one it inherits.
struct ClearColor {
    f32 r = 0.0F;
    f32 g = 0.0F;
    f32 b = 0.0F;
    f32 a = 1.0F;

    constexpr bool operator==(const ClearColor&) const = default;
};

/// One colour attachment of a rendering pass.
struct ColorAttachment {
    TextureHandle texture = {};
    LoadOp        loadOp  = LoadOp::Load;
    StoreOp       storeOp = StoreOp::Store;

    /// Read only when `loadOp` is `LoadOp::Clear`, and ignored otherwise.
    ClearColor clearValue = {};
};

/// Largest number of colour attachments `ICommandList::BeginRendering` accepts.
///
/// Eight is D3D12's fixed `D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT`, and both adapters on the
/// development machine report `maxColorAttachments = 8` (measured with `vulkaninfo`).
///
/// **It is not a claim that every device offers eight.** Vulkan's *required* minimum is lower,
/// so this is a bound on the fixed array `BeginRendering` builds rather than a capability. The
/// first caller that uses more than one attachment is also the first with a reason to ask the
/// device its own limit, and that question belongs with the capability tiers in
/// Capabilities.h -- where a field is only added when a requirement reads it.
inline constexpr usize kMaxColorAttachments = 8;

/// A rendering pass, as Vulkan 1.3 dynamic rendering and D3D12 both express one: attachments,
/// a render area, and nothing that has to be created ahead of time.
///
/// **No render pass object and no framebuffer object, anywhere.** That is not an omission from
/// this struct, it is the decision -- see `ICommandList::BeginRendering`.
struct RenderingDescription {
    /// The area rendered to, from the origin. A separate offset arrives with the first
    /// caller that renders to part of a texture.
    Extent2D extent = {};

    /// Read only for the duration of the `BeginRendering` call; nothing about it is stored.
    /// At most `kMaxColorAttachments` entries.
    ///
    /// Every attachment must already be in `TextureLayout::ColorAttachment` -- the barrier
    /// that puts it there is the caller's, because only the caller knows what the texture was
    /// doing before. A depth attachment arrives with the first pass that needs one.
    std::span<const ColorAttachment> colorAttachments = {};
};

/// A texture to create.
struct TextureDescription {
    Extent2D     extent = {};
    Format       format = Format::Unknown;
    TextureUsage usage  = TextureUsage::None;
};

/// A buffer to create.
struct BufferDescription {
    u64            size     = 0;
    BufferUsage    usage    = BufferUsage::None;
    MemoryLocation location = MemoryLocation::DeviceLocal;
};

/// How many resources of each kind a device's pools hold.
///
/// **Fixed at device creation and never grown**, which is `JobSystem::Config`'s discipline and
/// is here for the same reason: creation must not touch the allocator on a path a frame runs.
/// A pool that is full reports `ErrorCode::OutOfMemory` rather than reallocating and
/// invalidating every handle that named it.
///
/// The defaults are small because A3 creates one texture and one buffer. The number that will
/// matter is Phase B's, once there is a scene to size it from.
struct DeviceConfig {
    u32 maxTextures = 64;
    u32 maxBuffers  = 64;
};

/// Records GPU commands. Obtained from `IDevice::BeginFrame`; never owned by the caller.
///
/// A command list is not thread-safe and is not meant to be: parallel recording, and more than
/// one list per frame, are A4's -- a render graph is the thing that has independent work to
/// record. Building for them now would mean designing a pool allocator for lists with no
/// caller to shape it.
///
/// Every recording call refuses politely on a list that is not recording, rather than
/// asserting: `Begin` is what starts recording and `End` is what stops it, and a
/// `[[nodiscard]] Status` is how a caller finds out it got the order wrong.
class ICommandList {
public:
    virtual ~ICommandList() = default;

    ICommandList(const ICommandList&)            = delete;
    ICommandList& operator=(const ICommandList&) = delete;

    /// Starts recording. Fails with `ErrorCode::InvalidArgument` if this list is already
    /// recording.
    [[nodiscard]] virtual Status Begin() = 0;

    /// Stops recording, leaving the list ready to submit. Fails with
    /// `ErrorCode::InvalidArgument` if this list is not recording, or if a rendering pass was
    /// begun and never ended.
    [[nodiscard]] virtual Status End() = 0;

    /// Records one barrier.
    ///
    /// One barrier per call, and one call per API barrier command. Batching several into one
    /// command is what a render graph does, because a render graph is what has several at once
    /// to batch; a batching overload taking a span is an addition beside these rather than a
    /// change to them, so nothing recorded through these three has to move when it arrives.
    ///
    /// A barrier that changes nothing is recorded rather than dropped. `GlobalBarrier{}` and a
    /// `TextureBarrier` with equal layouts are values a caller can mean -- see Barrier.h --
    /// and a command list that quietly skipped them would be deciding on the caller's behalf
    /// with less information than the caller had.
    ///
    /// **A stale or unknown handle here is reported through the assertion handler and the log
    /// line beside it, and the process does not continue past the call (see below), where
    /// `BeginRendering` and `CopyTextureToBuffer` return `ErrorCode::InvalidArgument` for the
    /// same condition and their caller carries on. That asymmetry follows from the
    /// void return and from nothing else** -- not from a judgement that a bad handle matters
    /// more in a barrier than in a copy. These three return void because Vulkan's and D3D12's
    /// barrier commands do, which is the shape a render graph batching hundreds of them per
    /// frame needs; a `Status` nobody could act on per barrier would be checked once and then
    /// ignored. With no return value to carry a report, the remaining options are to record a
    /// barrier naming a resource that does not exist -- undefined behaviour, and a validation
    /// error on the backend that has one -- or to refuse and say so out of band. It refuses.
    ///
    /// **The refusal ends the process, and not at the handler's discretion.** `MONARC_CHECK`
    /// alters no control flow of its own, so the check is followed by an unconditional
    /// `MONARC_DEBUG_BREAK(); std::abort();` -- the house pattern from `JobSystem::Wait`
    /// (see Docs/Runtime/Threading.md) and from the Vulkan backend's debug messenger, applied
    /// here for the same reason. Without it, a handler that declines to break --
    /// Shipping, or any test harness -- would get the report and then a *skipped barrier*, and
    /// the rest of the frame would record as though the barrier had been asked for. A dropped
    /// barrier is not a refused operation: it is a synchronisation hole whose symptom is wrong
    /// pixels or a hang on some driver, and nothing in the resulting capture points back to
    /// the call that caused it. `BeginRendering` and `CopyTextureToBuffer` do not need this
    /// because their `Status` reaches the caller in every configuration.
    ///
    /// Measured, both ways, out of process, because in process there is nothing left to
    /// measure with: a program that installs a handler returning `false` and issues a barrier
    /// against a handle a real `DestroyTexture` made stale ends at `0x80000003` -- the debug
    /// break, which is what stops it before `std::abort()` is reached -- and its next line
    /// never runs; with the `return` this replaced, the same program reached that line and
    /// exited zero. Docs/Status.md records both runs. The same fact is why the log line's
    /// content is composed by `Describe` in Barrier.h rather than open-coded in the backend:
    /// no test can survive the call to read what was logged, so the composition is tested
    /// where it needs neither a device nor Vulkan.
    ///
    /// What is *not* asymmetric is the part ADR-0002 requires: all three paths **detect** the
    /// stale handle rather than dereferencing it. The difference is only in what they can say
    /// afterwards. If a batching overload arrives with a `Status`, it reports like the other
    /// two and this note goes with it.
    virtual void Barrier(const GlobalBarrier& barrier) = 0;
    virtual void Barrier(const BufferBarrier& barrier) = 0;
    virtual void Barrier(const TextureBarrier& barrier) = 0;

    /// Begins a rendering pass, applying each attachment's load-op.
    ///
    /// **Vulkan 1.3 dynamic rendering, with no `VkRenderPass` and no `VkFramebuffer` anywhere
    /// in the backend.** Attachments are named at record time, so there is no object to create
    /// ahead of a frame, nothing to cache and invalidate when a texture is recreated, and no
    /// compatibility rules between a pipeline and a pass. D3D12 and Metal both work this way
    /// already, so the model is the one all three share rather than Vulkan's older shape
    /// emulated on the other two.
    ///
    /// Every attachment's texture must have been created with `TextureUsage::ColorAttachment`.
    /// A texture without it has nothing for a backend to render into -- a Vulkan image view
    /// cannot even be created for a transfer-only image -- so this is a refusal and not a
    /// silent no-op.
    ///
    /// Fails with `ErrorCode::InvalidArgument` for a list that is not recording, for a pass
    /// begun inside another, for more than `kMaxColorAttachments` attachments, for an empty
    /// extent, for an attachment without `TextureUsage::ColorAttachment`, or for an attachment
    /// naming a texture this device does not have -- including a handle whose generation is
    /// stale, which is the whole point of ADR-0002's generation counter. That last one is
    /// *reported* here and asserted in `Barrier`; see `Barrier` for why the return type and
    /// not the severity is what decides which.
    [[nodiscard]] virtual Status BeginRendering(const RenderingDescription& description) = 0;

    /// Ends the rendering pass `BeginRendering` began.
    virtual void EndRendering() = 0;

    /// Copies the whole of `source` into `destination`, tightly packed: rows follow one
    /// another with no padding, so the byte at `(x, y)` is at
    /// `(y * width + x) * BytesPerPixel(format)`.
    ///
    /// `source` must be in `TextureLayout::TransferSource` and `destination` must be at least
    /// that many bytes and have `BufferUsage::TransferDestination`.
    ///
    /// Whole-texture rather than a region, and one mip level, because that is what A3 needs
    /// and a region struct nothing fills would be a guess about mip chains and sub-rect
    /// uploads that a later phase has to undo. A region-taking overload is an addition.
    ///
    /// Fails with `ErrorCode::InvalidArgument` for a list that is not recording, inside a
    /// rendering pass, for either handle being unknown or stale, or for a destination too
    /// small to hold the texture. `BeginRendering`'s note on the stale-handle case applies
    /// here unchanged.
    [[nodiscard]] virtual Status CopyTextureToBuffer(TextureHandle source,
                                                     BufferHandle  destination) = 0;

protected:
    ICommandList() = default;

    // Protected and defaulted rather than absent. Deleting the copy operations above suppresses
    // the implicit move operations too, so a derived class that needs to be movable would find
    // the base's *deleted copy* constructor instead of a move. `VulkanDevice` is the one that
    // needs it -- it is returned by value from a factory -- and declaring these on all three
    // interfaces keeps movability a decision the derived class makes rather than one the
    // interface forecloses.
    ICommandList(ICommandList&&)            = default;
    ICommandList& operator=(ICommandList&&) = default;
};

/// A queue commands are submitted to, and the timeline that says when they finished.
///
/// **One timeline semaphore, and no binary semaphores.** Timeline semaphores are Vulkan 1.3
/// core and are the right tool for "has the work I recorded two frames ago finished": the
/// value is monotonic, the CPU can wait on or poll any value, and there is no per-frame object
/// to create and recycle. Binary semaphores arrive in Task 4, because `vkQueuePresentKHR`
/// accepts only those -- using each where it belongs is honest, and pretending the timeline
/// covers presentation would not survive contact with a swapchain.
///
/// One queue, because A3 has one. Multiple queues in anger are excluded from A3 by name.
class IQueue {
public:
    virtual ~IQueue() = default;

    IQueue(const IQueue&)            = delete;
    IQueue& operator=(const IQueue&) = delete;

    /// Submits `commands` and returns the timeline value this submission will signal when it
    /// completes. The value is greater than every value returned before it on this queue.
    ///
    /// `commands` must have been `End`ed and must have come from this queue's own device --
    /// a list from another device is `ErrorCode::InvalidArgument`, not undefined behaviour,
    /// which matters on a machine with two adapters and therefore two devices.
    [[nodiscard]] virtual Result<u64> Submit(ICommandList& commands) = 0;

    /// Blocks until the timeline has reached `value`, or until `timeoutNanoseconds` elapses.
    /// A timeout is `ErrorCode::BackendFailure` and not success: a caller that read a buffer
    /// anyway would be reading memory the GPU is still writing.
    [[nodiscard]] virtual Status Wait(u64 value, u64 timeoutNanoseconds) = 0;

    /// The highest timeline value the GPU has actually reached. Never blocks.
    [[nodiscard]] virtual Result<u64> CompletedValue() const = 0;

    /// The value the most recent `Submit` will signal, or zero if nothing has been submitted.
    /// The timeline starts at zero and no submission signals it, so zero unambiguously means
    /// "nothing submitted".
    ///
    /// **These two have no caller in shipped code yet.** Their only caller today is the device
    /// test, which is an exception to "a member arrives with its first user" and is stated
    /// rather than left to be noticed. What they buy is two questions nothing else here can
    /// ask:
    ///
    /// - **What value has the timeline actually reached, without blocking to find out.**
    ///   `Wait` answers "did it reach *this* value", and answers it definitively -- a timeout
    ///   is `ErrorCode::BackendFailure` and never success -- but it answers by blocking and it
    ///   never reports the current value. So `Wait` cannot say that a fresh device's timeline
    ///   is at zero: waiting for zero succeeds at once whatever the timeline holds. Only
    ///   `CompletedValue` can. A frame loop asking whether frame N-2 has finished without
    ///   stalling -- frame pacing, which Docs/Rendering/RHI.md defers -- asks the same
    ///   question.
    /// - **Whether the queue itself remembers what it handed out.** `Submit` returns the value,
    ///   so monotonicity across submissions is assertable without either of these; what is not
    ///   is that the queue's own record agrees with the value it returned, or that it reads
    ///   zero before anything is submitted -- which is the fact `BeginFrame`'s "zero means
    ///   never submitted" wait depends on.
    ///
    /// Both also answer on a shut-down or moved-from device rather than dereferencing:
    /// `LastSubmittedValue` reads zero, `CompletedValue` reports `ErrorCode::InvalidArgument`.
    [[nodiscard]] virtual u64 LastSubmittedValue() const = 0;

protected:
    IQueue() = default;

    IQueue(IQueue&&)            = default;
    IQueue& operator=(IQueue&&) = default;
};

/// A logical device: one adapter, one graphics queue, the resources created on it, and the
/// per-frame command pools commands are recorded into.
///
/// **This is the interface `Monarc.Render` consumes in Phase B**, so it is the real intended
/// shape rather than a step towards one. A call site holding an `IDevice&` should not have to
/// change.
///
/// **There is deliberately no `IBackend` yet, and no owning handle for an `IDevice`.** The
/// A3 plan's file listing names a `Backend.h`; Task 3's checkboxes name these three interfaces
/// and not that one, and the difference is the reason. An `IBackend`'s only justification for
/// being virtual is runtime backend selection, and there is one backend -- so
/// `VulkanBackend::CreateDevice` returns a concrete `Result<VulkanDevice>`, which is
/// polymorphic *use* without polymorphic *ownership*. Polymorphic ownership needs an owning
/// pointer type in `Monarc.Core`, and that is its own design: `IAllocator::Deallocate` needs
/// the size and alignment of what it is freeing, and for a polymorphic type those are the
/// derived class's, not `sizeof(Base)` -- so the deleter has to carry values captured where
/// the object was made.
///
/// Both arrive with the second backend, which ADR-0012 schedules immediately after M0. **The
/// interfaces in this header do not change when they do**; what changes is the factory's
/// return type. That is a stated seam rather than an unstated one.
class IDevice {
public:
    virtual ~IDevice() = default;

    IDevice(const IDevice&)            = delete;
    IDevice& operator=(const IDevice&) = delete;

    /// The adapter this device was created on, as enumeration described it.
    [[nodiscard]] virtual const AdapterInfo& Adapter() const = 0;

    /// The device's graphics queue. Owned by the device and valid for as long as it is.
    [[nodiscard]] virtual IQueue& GraphicsQueue() = 0;

    /// Creates a texture and returns a handle naming its pool slot.
    ///
    /// A description rather than a parameter list, and a handle rather than a pointer: the
    /// first is what lets the memory strategy behind this change without touching a call site
    /// (see the note on allocation in Monarc.RHI.Vulkan/Include/.../VulkanDevice.h), and the
    /// second is ADR-0002.
    ///
    /// Fails with `ErrorCode::InvalidArgument` for an empty extent, `Format::Unknown` or no
    /// usage; `ErrorCode::OutOfMemory` when every one of `DeviceConfig::maxTextures` slots is
    /// occupied; and `ErrorCode::BackendFailure` or `ErrorCode::Unsupported` for what the API
    /// said.
    [[nodiscard]] virtual Result<TextureHandle> CreateTexture(
        const TextureDescription& description) = 0;

    /// Destroys the texture `texture` names and frees its slot for reuse, bumping the slot's
    /// generation so that every handle to it is stale from this call onwards rather than from
    /// the next creation onwards.
    ///
    /// **The timing is the guarantee, and it is the reason the bump is here at all.** A slot
    /// whose generation moved only when it was next claimed would leave a window -- destroyed,
    /// not yet reclaimed -- in which a stale handle's generation still matched, and a backend's
    /// free/occupied flag would be the only thing refusing it. That was measured to be a real
    /// hole rather than a theoretical one; see `TextureSlot::generation` in
    /// Monarc.RHI.Vulkan/Private/VulkanDevice.cpp for the measurement and for what it cost.
    ///
    /// Safe on an invalid handle and on one already destroyed: both are no-ops that bump
    /// nothing, because "destroy what may or may not still exist" is what a teardown path
    /// actually has.
    ///
    /// **The caller must ensure the GPU is finished with it.** Nothing here tracks in-flight
    /// use; `IQueue::Wait` is how a caller knows.
    virtual void DestroyTexture(TextureHandle texture) = 0;

    /// Creates a buffer. `CreateTexture`'s failures apply, with `DeviceConfig::maxBuffers`.
    [[nodiscard]] virtual Result<BufferHandle> CreateBuffer(
        const BufferDescription& description) = 0;

    /// `DestroyTexture` for buffers: the same generation bump with the same timing, the same
    /// tolerance of stale handles, and the same requirement about in-flight use.
    virtual void DestroyBuffer(BufferHandle buffer) = 0;

    /// Maps a `MemoryLocation::HostVisible` buffer and returns a read-only view of the whole
    /// of it. The view is valid until `UnmapBuffer`.
    ///
    /// Read-only because A3 reads back and does not upload. An upload path returning a
    /// writable span arrives with the first vertex buffer.
    ///
    /// Fails with `ErrorCode::InvalidArgument` for an unknown or stale handle or for one
    /// already mapped, and `ErrorCode::Unsupported` for a device-local buffer.
    [[nodiscard]] virtual Result<std::span<const u8>> MapBufferForRead(BufferHandle buffer) = 0;

    /// Unmaps a buffer `MapBufferForRead` mapped, invalidating the span it returned. Safe on a
    /// buffer that is not mapped and on a stale handle.
    virtual void UnmapBuffer(BufferHandle buffer) = 0;

    /// Advances to the next frame slot and hands back its command list, reset and ready for
    /// `ICommandList::Begin`.
    ///
    /// **This is where the frame boundary is, and with no swapchain it is the only place it
    /// could be.** A frame here is one slot of `kFramesInFlight`, each with its own command
    /// pool. This call blocks until the GPU has passed the timeline value that slot's previous
    /// submission signalled -- which is what makes resetting the pool safe, since resetting a
    /// pool whose buffers are still executing is undefined -- and then resets it. Task 4 adds
    /// acquire and present either side of this; it does not move the wait, because the wait is
    /// about the pool and not about the image.
    ///
    /// Never null on success. The returned list belongs to the device and is valid until the
    /// next `BeginFrame` for the same slot, so a caller must not hold one across frames.
    [[nodiscard]] virtual Result<ICommandList*> BeginFrame() = 0;

    /// Blocks until the device is idle -- every queue, every submission. What a teardown path
    /// calls before destroying resources, and the only wait in this interface that is not
    /// about a timeline value.
    [[nodiscard]] virtual Status WaitIdle() = 0;

protected:
    IDevice() = default;

    // See ICommandList's own note. A VulkanDevice is returned by value from a factory, so
    // unlike the two interfaces above this one genuinely needs its move operations to exist.
    IDevice(IDevice&&)            = default;
    IDevice& operator=(IDevice&&) = default;
};

}  // namespace Monarc::RHI
