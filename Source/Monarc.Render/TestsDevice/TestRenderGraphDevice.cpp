// Device-required tests for the render graph, with no window anywhere in them.
//
// **The point of this file is that the graph needs a driver and not a display.** Everything
// Phase A4 built above `Execute` is a pure function of the declarations and is tested in
// Tests/, device-free, against the stub `RHI::ICommandList` in Private/TestSupport/. What that
// cannot say is whether a real Vulkan implementation accepts the barriers the derivation
// produced and produces the pixels the frame asked for. That needs a device -- and it needs
// nothing else: a texture the test creates, a graph that clears it through a declared
// attachment, and a copy back into a mapped buffer. No surface, no swapchain, no presentation
// and no window.
//
// **Which is also why this suite is here and the swapchain's is not.** `Monarc.Render` is tier 2
// and may not see `Monarc.Host.Windowed`, which is tier 3 -- gate 14 in Tools/check_architecture.py
// polices exactly that on a test target's link line. Linking `Monarc.RHI.Vulkan` is the allowed
// direction, tier 2 to tier 2, and is what the CMakeLists.txt beside this directory does. The
// swapchain-through-the-graph case therefore lives in Monarc.Host.Windowed/TestsDevice/, beside
// the window it needs.
//
// **A missing device is a SKIP, never a PASS.** main() below returns 77 -- CTest's
// `SKIP_RETURN_CODE`, set by `monarc_device_test_module()` -- when there is no Vulkan runtime,
// the instance will not come up, or the machine reports no adapters, and it decides that
// *before doctest runs a case*. Monarc.RHI.Vulkan/TestsDevice/TestVulkanDevice.cpp gives the
// reason and this file follows it exactly: a filter inside a binary that had already started
// would report "0 tests, all passed". CI has a Vulkan loader and no ICD, so this is the path CI
// takes.
//
// **What "runs clean" means here, stated because it is weaker than it sounds.** The Debug
// builds install a fatal debug messenger, so any VALIDATION-type message from the layer stops
// the process; a case that returns has therefore produced none. The validation layer checks a
// barrier's *legality* -- that a layout is permitted for the image's usage, that the stage and
// access masks pair, that a transition's old layout matches what the image is in. It does not
// check *sufficiency*: a frame missing a barrier it needed is not a validation error, it is a
// race. Synchronization validation is what checks that, and Monarc does not enable it -- so
// nothing in this file proves a derived barrier was *necessary*, only that the whole frame was
// accepted and produced the expected bytes.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Log.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/RHI/Adapter.h>
#include <Monarc/RHI/Barrier.h>
#include <Monarc/RHI/Device.h>
#include <Monarc/RHI/Types.h>
#include <Monarc/RHI/Vulkan/VulkanBackend.h>
#include <Monarc/RHI/Vulkan/VulkanDevice.h>
#include <Monarc/Render/RenderGraph.h>

#include <span>
#include <string_view>

namespace {

MONARC_LOG_CATEGORY(RenderGraphDeviceTest, Info);

/// CTest's SKIP_RETURN_CODE for this binary, set by `monarc_device_test_module()` in
/// Source/Monarc.Render/CMakeLists.txt. 77 is autotools' long-standing convention for the same
/// thing, and the value every other device suite in this tree uses.
constexpr int kSkipReturnCode = 77;

constexpr std::string_view kLibraryOption = "--vulkan-library=";

/// How long a wait on the GPU timeline is given before it is called a deadlock. Five seconds,
/// matching the backend's own `kTimelineWaitTimeoutNanoseconds`: a suite that hangs forever
/// tells CI nothing.
constexpr Monarc::u64 kWaitTimeoutNanoseconds = 5'000'000'000ULL;

/// The colour every frame here clears to, and the bytes it must read back as.
///
/// **The same three numbers Monarc.FirstLight clears to and the same ones A3's readbacks
/// assert**, so all of them are statements about one colour rather than four coincidences. Each
/// of 64, 128 and 192 over 255 survives the round trip through `f32` and back through UNORM
/// quantisation, so the driver has no rounding decision to make -- which is what lets this
/// assert exact bytes. They are also far apart and asymmetric, so a channel order swapped
/// between R and B is a failure rather than a plausible near miss.
///
/// The byte order is the *texture's*: `R8G8B8A8_UNORM` stores red first, so these read back in
/// the order they were written. Monarc.Host.Windowed's swapchain readback asserts the same clear
/// through a `B8G8R8A8_UNORM` image and therefore expects `(192, 128, 64, 255)`; the difference
/// is the format and not the clear.
constexpr Monarc::u8 kExpectedBytes[4] = {64, 128, 192, 255};

constexpr Monarc::RHI::ClearColor kClearColor{
    static_cast<Monarc::f32>(kExpectedBytes[0]) / 255.0F,
    static_cast<Monarc::f32>(kExpectedBytes[1]) / 255.0F,
    static_cast<Monarc::f32>(kExpectedBytes[2]) / 255.0F,
    static_cast<Monarc::f32>(kExpectedBytes[3]) / 255.0F};

/// The target's size. Four by four rather than one by one, for
/// Monarc.RHI.Vulkan/TestsDevice/TestVulkanDevice.cpp's reason: a single pixel would pass with a
/// copy whose row pitch was wrong, and sixteen is enough for a row-stride mistake to land the
/// second row's bytes somewhere this test looks.
constexpr Monarc::RHI::Extent2D kExtent{4, 4};

constexpr Monarc::RHI::Format kFormat = Monarc::RHI::Format::R8G8B8A8_UNORM;

/// What a frame's output texture is created with: renderable, and copyable out of.
constexpr Monarc::RHI::TextureUsage kOutputUsage =
    Monarc::RHI::TextureUsage::ColorAttachment | Monarc::RHI::TextureUsage::TransferSource;

/// The description the graph is handed for the imported output. **The same values
/// `IDevice::CreateTexture` was given**, and it has to be: `PassBuilder::ColorAttachment`
/// refuses a resource whose usage lacks `ColorAttachment`, and `RenderGraph::Execute` builds the
/// rendering instance's render area out of the extent.
constexpr Monarc::RHI::TextureDescription kOutputDescription{kExtent, kFormat, kOutputUsage};

/// The state an offscreen texture the test just created arrives in.
///
/// `Undefined` because that is the layout `IDevice::CreateTexture` leaves a texture in, and
/// transitioning out of it discards whatever the memory held -- correct, because the next thing
/// is a clear. `PipelineStage::None` and `Access::None` because nothing has touched it: there is
/// no earlier submission to order against.
///
/// **This differs from `Monarc.FirstLight`'s incoming state in exactly one field, and the
/// difference is the swapchain.** That import names `ColorAttachmentOutput` on the before side
/// because `VulkanDeviceState::SubmitList` waits on the acquire semaphore at that stage and the
/// transition has to be ordered after that wait. There is no acquire here and no semaphore, so
/// there is nothing for the before scope to chain to.
constexpr Monarc::RHI::TextureState kFreshTexture{Monarc::RHI::TextureLayout::Undefined,
                                                  Monarc::RHI::PipelineStage::None,
                                                  Monarc::RHI::Access::None};

/// The state the graph must leave the output in so that the next submission can copy it out.
///
/// **This is how the readback happens without the graph knowing what a buffer is.** A4's
/// `ResourceAccess` has no transfer access, so no *pass* can ask for `TransferSource` -- but an
/// import's outgoing state is stated in `RHI` vocabulary rather than in `ResourceAccess`, so it
/// can. The graph's closing barrier is then the same `ColorAttachment` to `TransferSource`
/// transition A3's readback hand-wrote, and the copy in the following submission needs no
/// barrier of its own: this one already made the attachment write available and visible to
/// `TransferRead` at the `Copy` stage.
constexpr Monarc::RHI::TextureState kReadyToCopy{Monarc::RHI::TextureLayout::TransferSource,
                                                 Monarc::RHI::PipelineStage::Copy,
                                                 Monarc::RHI::Access::TransferRead};

/// The backend main() brought up, and the adapters it found, deduplicated. Raw pointers to
/// locals in main rather than static objects, so nothing Vulkan-shaped is constructed during
/// static initialisation and the teardown order at exit is main's rather than the linker's --
/// TestVulkanDevice.cpp's arrangement, for its reason.
Monarc::RHI::VulkanBackend*              g_backend  = nullptr;
Monarc::Array<Monarc::RHI::AdapterInfo>* g_adapters = nullptr;

[[nodiscard]] Monarc::RHI::VulkanBackend& Backend() {
    MONARC_CHECK(g_backend != nullptr, "the render graph device test's backend was never up");
    return *g_backend;
}

[[nodiscard]] const Monarc::Array<Monarc::RHI::AdapterInfo>& Adapters() { return *g_adapters; }

[[nodiscard]] Monarc::u64 ReadbackByteCount() {
    return static_cast<Monarc::u64>(kExtent.width) * kExtent.height *
           Monarc::RHI::BytesPerPixel(kFormat);
}

/// A device on one adapter, shut down in the destructor whatever the case did.
///
/// Opened by a constructor rather than by an `Open` call because there is exactly one fallible
/// step: `Ready()` says whether it took.
struct DeviceUnderTest {
    Monarc::SystemAllocator                   allocator;
    Monarc::Result<Monarc::RHI::VulkanDevice> device = Monarc::Err(Monarc::ErrorCode::Unknown);

    explicit DeviceUnderTest(const Monarc::RHI::AdapterInfo& adapter)
        : device(Backend().CreateDevice(allocator, adapter, Monarc::RHI::DeviceConfig{})) {}

    ~DeviceUnderTest() {
        if (device) {
            device->Shutdown();
        }
    }

    DeviceUnderTest(const DeviceUnderTest&)            = delete;
    DeviceUnderTest& operator=(const DeviceUnderTest&) = delete;
    DeviceUnderTest(DeviceUnderTest&&)                 = delete;
    DeviceUnderTest& operator=(DeviceUnderTest&&)      = delete;

    [[nodiscard]] bool Ready() const { return device.has_value(); }
};

/// Copies `texture` into `staging` and waits for the bytes to be there.
///
/// **A second submission, and that is forced by the graph's shape rather than chosen.**
/// `RenderGraph::Execute` calls `ICommandList::Begin` and `End` itself, so nothing can be
/// appended to the frame it recorded; and A4's graph cannot record the copy *inside* the frame
/// either, for three separate reasons, each of which would have to go: `ResourceAccess` has no
/// transfer access, so no pass can ask for the `TransferSource` layout; `PassCommandList`
/// forwards no recording call, so no pass callback can issue a copy; and culling drops a pass
/// that only reads an import, so a readback pass whose output is a buffer the graph does not
/// model would be culled before it ran.
///
/// **No texture barrier is recorded here**, which is the part worth checking against: the
/// graph's own closing barrier left the image in `TransferSource` with the attachment write made
/// visible to `TransferRead`, and submission order on one queue is what orders this list after
/// that one. The only barrier below is about the *buffer*, which the graph has no concept of.
[[nodiscard]] Monarc::Status CopyOut(Monarc::RHI::IDevice& device, Monarc::RHI::TextureHandle
                                                                       texture,
                                     Monarc::RHI::BufferHandle staging) {
    const Monarc::Result<Monarc::RHI::ICommandList*> commands = device.BeginFrame();
    if (!commands) {
        return Monarc::Status(std::unexpect, commands.error());
    }
    Monarc::RHI::ICommandList& list = **commands;

    if (Monarc::Status begun = list.Begin(); !begun) {
        return begun;
    }
    if (Monarc::Status copied = list.CopyTextureToBuffer(texture, staging); !copied) {
        return copied;
    }

    // Waiting on the timeline makes the copy's writes *available*; it does not make them visible
    // to the host. A memory dependency into the host stage is what does, and without it a mapped
    // read is reading memory whose visibility nothing established.
    list.Barrier(Monarc::RHI::BufferBarrier{staging, Monarc::RHI::PipelineStage::Copy,
                                            Monarc::RHI::PipelineStage::Host,
                                            Monarc::RHI::Access::TransferWrite,
                                            Monarc::RHI::Access::HostRead});

    if (Monarc::Status ended = list.End(); !ended) {
        return ended;
    }

    const Monarc::Result<Monarc::u64> submitted = device.GraphicsQueue().Submit(list);
    if (!submitted) {
        return Monarc::Status(std::unexpect, submitted.error());
    }
    return device.GraphicsQueue().Wait(*submitted, kWaitTimeoutNanoseconds);
}

/// Asserts that every pixel of `bytes` is the clear colour, and logs what was read.
void CheckIsClearColour(const char* what, const char* adapterName,
                        std::span<const Monarc::u8> bytes) {
    REQUIRE(bytes.size() == ReadbackByteCount());

    MONARC_LOG(RenderGraphDeviceTest, Info,
               "{} on \"{}\": first pixel = ({}, {}, {}, {}), expected ({}, {}, {}, {})", what,
               adapterName, bytes[0], bytes[1], bytes[2], bytes[3], kExpectedBytes[0],
               kExpectedBytes[1], kExpectedBytes[2], kExpectedBytes[3]);

    CHECK(bytes[0] == kExpectedBytes[0]);
    CHECK(bytes[1] == kExpectedBytes[1]);
    CHECK(bytes[2] == kExpectedBytes[2]);
    CHECK(bytes[3] == kExpectedBytes[3]);

    // Every pixel, individually. The first-pixel assertion is the headline and this loop is what
    // catches a copy whose row pitch was wrong.
    Monarc::usize       matching = 0;
    const Monarc::usize pixels   = bytes.size() / 4;
    for (Monarc::usize pixel = 0; pixel < pixels; ++pixel) {
        const Monarc::u8* p = bytes.data() + pixel * 4;
        if (p[0] == kExpectedBytes[0] && p[1] == kExpectedBytes[1] && p[2] == kExpectedBytes[2] &&
            p[3] == kExpectedBytes[3]) {
            ++matching;
        }
    }
    MONARC_LOG(RenderGraphDeviceTest, Info, "{} on \"{}\": {} of {} pixel(s) exact", what,
               adapterName, matching, pixels);
    CHECK(matching == pixels);
}

}  // namespace

TEST_CASE("THE GRAPH READBACK: a frame the graph recorded clears to the exact bytes") {
    // **The headless half of the phase's headline, and it runs on every deduplicated adapter.**
    // One pass, one imported texture, one attachment with a clear load-op, and no barrier
    // written by this file at all: every transition below was derived by `Compile` and recorded
    // by `Execute`. A result that differs between vendors is a finding and not a flake, so every
    // pixel is asserted and the bytes are logged per adapter rather than only on failure.
    REQUIRE_FALSE(Adapters().IsEmpty());

    for (const Monarc::RHI::AdapterInfo& adapter : Adapters()) {
        DeviceUnderTest held(adapter);
        REQUIRE(held.Ready());
        Monarc::RHI::IDevice& device = *held.device;

        const Monarc::Result<Monarc::RHI::TextureHandle> texture =
            device.CreateTexture(kOutputDescription);
        REQUIRE(texture.has_value());

        Monarc::RHI::BufferDescription bufferDescription{};
        bufferDescription.size     = ReadbackByteCount();
        bufferDescription.usage    = Monarc::RHI::BufferUsage::TransferDestination;
        bufferDescription.location = Monarc::RHI::MemoryLocation::HostVisible;
        const Monarc::Result<Monarc::RHI::BufferHandle> staging =
            device.CreateBuffer(bufferDescription);
        REQUIRE(staging.has_value());

        Monarc::Render::RenderGraph graph(held.allocator, Monarc::Render::RenderGraph::Config{});

        Monarc::Result<Monarc::Render::PassBuilder> pass = graph.AddPass("clear");
        REQUIRE(pass.has_value());

        const Monarc::Result<Monarc::Render::TextureId> target = pass->ImportTexture(
            "output", Monarc::Render::TextureImport(*texture, kOutputDescription, kFreshTexture,
                                                    kReadyToCopy));
        REQUIRE(target.has_value());
        REQUIRE(pass->ColorAttachment(*target, Monarc::RHI::LoadOp::Clear,
                                      Monarc::RHI::StoreOp::Store, kClearColor)
                    .has_value());
        REQUIRE(graph.Compile().has_value());

        // Two barriers and no more: the two A3 hand-wrote for its offscreen readback. Asserted
        // through inspection as well as run, so that a frame which silently stopped emitting one
        // of them would fail here rather than depend on a driver noticing.
        const Monarc::Render::GraphInspection report = graph.Inspect();
        REQUIRE(report.barriers.size() == 2);
        CHECK(report.barriers[0].layoutBefore == Monarc::RHI::TextureLayout::Undefined);
        CHECK(report.barriers[0].layoutAfter == Monarc::RHI::TextureLayout::ColorAttachment);
        CHECK(report.barriers[1].layoutBefore == Monarc::RHI::TextureLayout::ColorAttachment);
        CHECK(report.barriers[1].layoutAfter == Monarc::RHI::TextureLayout::TransferSource);

        const Monarc::Result<Monarc::RHI::ICommandList*> commands = device.BeginFrame();
        REQUIRE(commands.has_value());
        REQUIRE(graph.Execute(device, **commands).has_value());

        const Monarc::Result<Monarc::u64> submitted = device.GraphicsQueue().Submit(**commands);
        REQUIRE(submitted.has_value());
        REQUIRE(device.GraphicsQueue().Wait(*submitted, kWaitTimeoutNanoseconds).has_value());

        REQUIRE(CopyOut(device, *texture, *staging).has_value());

        const Monarc::Result<std::span<const Monarc::u8>> mapped =
            device.MapBufferForRead(*staging);
        REQUIRE(mapped.has_value());
        CheckIsClearColour("graph readback", adapter.name, *mapped);

        device.UnmapBuffer(*staging);
        REQUIRE(device.WaitIdle().has_value());
        device.DestroyBuffer(*staging);
        device.DestroyTexture(*texture);
    }
}

TEST_CASE("a transient written by one pass and read by another runs on a device") {
    // **The phase plan's third device requirement**: the read-after-write barrier the derivation
    // produced is one a driver accepts, in a frame that also produces the right pixels.
    //
    // The frame is two passes. The first clears a **transient** the graph creates and owns; the
    // second reads it and renders into the imported output. The derivation's answer for the
    // transient is a barrier between the two passes -- `ColorAttachment` on both sides, because
    // both accesses want that layout, with the write made available to the read.
    //
    // **What this cannot assert, stated rather than implied: the transient's own contents.**
    // Nothing outside the graph can name a transient's texture -- `GraphInspection` reports its
    // description and its lifetime, not its handle -- so there is no way to copy it out, and
    // A4 has no shader with which the second pass could consume what the first wrote. So the
    // read is declared and barriered and reads nothing. What is asserted is that the frame,
    // including that barrier, is accepted by a real driver with the fatal validation messenger
    // installed, and that the output pass's clear lands exactly. That is weaker than "the
    // barrier was necessary" -- see this file's head on legality versus sufficiency.
    REQUIRE_FALSE(Adapters().IsEmpty());

    for (const Monarc::RHI::AdapterInfo& adapter : Adapters()) {
        DeviceUnderTest held(adapter);
        REQUIRE(held.Ready());
        Monarc::RHI::IDevice& device = *held.device;

        const Monarc::Result<Monarc::RHI::TextureHandle> output =
            device.CreateTexture(kOutputDescription);
        REQUIRE(output.has_value());

        Monarc::RHI::BufferDescription bufferDescription{};
        bufferDescription.size     = ReadbackByteCount();
        bufferDescription.usage    = Monarc::RHI::BufferUsage::TransferDestination;
        bufferDescription.location = Monarc::RHI::MemoryLocation::HostVisible;
        const Monarc::Result<Monarc::RHI::BufferHandle> staging =
            device.CreateBuffer(bufferDescription);
        REQUIRE(staging.has_value());

        Monarc::Render::RenderGraph graph(held.allocator, Monarc::Render::RenderGraph::Config{});

        // The transient is renderable and nothing else: it is never copied out, because nothing
        // can name it, so asking for `TransferSource` would be a usage bit with no user.
        constexpr Monarc::RHI::TextureDescription kTransientDescription{
            kExtent, kFormat, Monarc::RHI::TextureUsage::ColorAttachment};

        Monarc::Result<Monarc::Render::PassBuilder> fill = graph.AddPass("fill the transient");
        REQUIRE(fill.has_value());
        const Monarc::Result<Monarc::Render::TextureId> scratch =
            fill->CreateTexture("scratch", kTransientDescription);
        REQUIRE(scratch.has_value());
        REQUIRE(fill->ColorAttachment(*scratch, Monarc::RHI::LoadOp::Clear,
                                      Monarc::RHI::StoreOp::Store, kClearColor)
                    .has_value());

        Monarc::Result<Monarc::Render::PassBuilder> compose = graph.AddPass("compose");
        REQUIRE(compose.has_value());
        const Monarc::Result<Monarc::Render::TextureId> target = compose->ImportTexture(
            "output", Monarc::Render::TextureImport(*output, kOutputDescription, kFreshTexture,
                                                    kReadyToCopy));
        REQUIRE(target.has_value());

        // **`Read` and not a second attachment**, which is what makes this a read rather than a
        // read-modify-write: an attachment always declares a write, so declaring the transient as
        // one would have given the second pass a write to it and the barrier a different after
        // scope. `ColorAttachmentRead` is the one read access whose layout a
        // `ColorAttachment`-usage image may legally be in -- `SampledRead` and `StorageRead` want
        // layouts that need usage bits `RHI::TextureUsage` does not have yet.
        REQUIRE(compose->Read(*scratch, Monarc::Render::ResourceAccess::ColorAttachmentRead)
                    .has_value());
        REQUIRE(compose
                    ->ColorAttachment(*target, Monarc::RHI::LoadOp::Clear,
                                      Monarc::RHI::StoreOp::Store, kClearColor)
                    .has_value());

        REQUIRE(graph.Compile().has_value());

        // Three barriers: the transient into the colour-attachment layout, the read-after-write
        // between the two passes, and the output's two -- four. Named individually rather than
        // counted, because a count says nothing about which is which.
        const Monarc::Render::GraphInspection report = graph.Inspect();
        REQUIRE(report.barriers.size() == 4);

        Monarc::usize readAfterWrite = report.barriers.size();
        for (Monarc::usize i = 0; i < report.barriers.size(); ++i) {
            const Monarc::Render::DerivedBarrier& barrier = report.barriers[i];
            if (barrier.resource == *scratch &&
                barrier.layoutBefore == Monarc::RHI::TextureLayout::ColorAttachment) {
                readAfterWrite = i;
            }
        }
        REQUIRE(readAfterWrite != report.barriers.size());
        CHECK(report.barriers[readAfterWrite].layoutAfter ==
              Monarc::RHI::TextureLayout::ColorAttachment);
        CHECK(report.barriers[readAfterWrite].accessBefore ==
              Monarc::RHI::Access::ColorAttachmentWrite);
        CHECK(report.barriers[readAfterWrite].accessAfter ==
              Monarc::RHI::Access::ColorAttachmentRead);

        const Monarc::Result<Monarc::RHI::ICommandList*> commands = device.BeginFrame();
        REQUIRE(commands.has_value());
        REQUIRE(graph.Execute(device, **commands).has_value());

        const Monarc::Result<Monarc::u64> submitted = device.GraphicsQueue().Submit(**commands);
        REQUIRE(submitted.has_value());
        REQUIRE(device.GraphicsQueue().Wait(*submitted, kWaitTimeoutNanoseconds).has_value());

        REQUIRE(CopyOut(device, *output, *staging).has_value());

        const Monarc::Result<std::span<const Monarc::u8>> mapped =
            device.MapBufferForRead(*staging);
        REQUIRE(mapped.has_value());
        CheckIsClearColour("transient frame readback", adapter.name, *mapped);
        device.UnmapBuffer(*staging);

        // ---------------------------------------------------------------------------------
        // **The teardown order `RenderGraph::Reset` requires, made visible because it is the
        // shipped contract.** `Reset` destroys the transient textures `Execute` created, and
        // `IDevice::DestroyTexture`'s precondition -- pushed up one level and stated on `Reset`
        // -- is that the caller has ensured the GPU is finished with the work that used them.
        // Nothing in the RHI tracks in-flight use; `IQueue::Wait` is how a caller knows, and the
        // wait above is the one that discharges it. Moving this `Reset` in front of that wait is
        // a real defect and not a style question: it destroys a `VkImage` a submitted command
        // buffer still references, which the validation layer reports and the Debug messenger
        // turns into a stopped process.
        //
        // This is also the graph whose device pointer is set, unlike `Monarc.FirstLight`'s: it
        // declared a transient, so it holds the `IDevice&` its `Execute` was given and must be
        // reset or destroyed before that device is. Doing it here, explicitly, rather than
        // leaving it to `graph`'s destructor is what makes the order something this file states
        // rather than something the declaration order happens to produce.
        // ---------------------------------------------------------------------------------
        REQUIRE(device.WaitIdle().has_value());
        graph.Reset();

        device.DestroyBuffer(*staging);
        device.DestroyTexture(*output);
    }
}

TEST_CASE("a build that asked for validation actually has a messenger") {
    // **The case that keeps this suite's "no validation output" claim from being vacuous.** Every
    // other case here reports nothing from the validation layer; that is worth something only if
    // the layer was loaded and a messenger was installed to hear it. A build that silently failed
    // to load the layer would otherwise pass as clean.
    //
    // The same case, for the same reason, is in
    // Monarc.Host.Windowed/TestsDevice/TestSwapchain.cpp and
    // Monarc.RHI.Vulkan/TestsDevice/TestVulkanDevice.cpp -- three suites, three separate
    // backends brought up in three separate processes, so each has to ask its own.
    //
    // Conditional on the build type rather than asserted outright, because Release deliberately
    // does not ask for validation: the implication being pinned is "asked for it, therefore has a
    // messenger", and asserting installation unconditionally would fail the Release presets for
    // doing the right thing.
    if (Backend().ValidationLayerEnabled()) {
        CHECK(Backend().DebugMessengerInstalled());
    } else {
        MONARC_LOG(RenderGraphDeviceTest, Info,
                   "validation was not enabled in this build, so no messenger is expected");
        CHECK_FALSE(Backend().DebugMessengerInstalled());
    }
}

int main(int argc, char** argv) {
    const char* libraryName = nullptr;

    // doctest's own option parsing would object to an argument it does not recognise, so
    // --vulkan-library is stripped out here and everything else is handed through unchanged.
    // Filtering rather than rejecting, so `ctest` and a developer running the binary by hand can
    // both still pass doctest's flags.
    Monarc::SystemAllocator allocator;
    Monarc::Array<char*>    forwarded(allocator);
    forwarded.Reserve(static_cast<Monarc::usize>(argc));
    for (int i = 0; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument.starts_with(kLibraryOption)) {
            libraryName = argv[i] + kLibraryOption.size();
            continue;
        }
        forwarded.Push(argv[i]);
    }

    Monarc::RHI::VulkanBackend::Config config{};
    config.applicationName = "Monarc.Render.DeviceTests";
    config.libraryName     = libraryName;

    Monarc::Result<Monarc::RHI::VulkanBackend> created =
        Monarc::RHI::VulkanBackend::Create(allocator, config);
    if (!created) {
        // Info, not Error: on a machine with no Vulkan this is the expected outcome and not a
        // fault, and CTest is about to print it as Skipped.
        MONARC_LOG(RenderGraphDeviceTest, Info,
                   "skipping the render graph device tests: {} -- {} (returning {} so CTest "
                   "reports Skipped rather than Passed)",
                   Monarc::ToString(created.error().code), created.error().message,
                   kSkipReturnCode);
        return kSkipReturnCode;
    }
    Monarc::RHI::VulkanBackend& backend = *created;

    Monarc::Array<Monarc::RHI::AdapterInfo> adapters(allocator);
    if (const Monarc::Status enumerated = backend.EnumerateAdapters(adapters); !enumerated) {
        MONARC_LOG(RenderGraphDeviceTest, Info,
                   "skipping the render graph device tests: enumeration failed -- {} -- {}",
                   Monarc::ToString(enumerated.error().code), enumerated.error().message);
        backend.Shutdown();
        return kSkipReturnCode;
    }

    if (adapters.IsEmpty()) {
        // A Vulkan loader with no ICD registered behind it: the instance comes up and there is
        // nothing to render on. This is what CI's Windows runners are.
        MONARC_LOG(RenderGraphDeviceTest, Info,
                   "skipping the render graph device tests: the Vulkan instance came up at "
                   "{}.{}.{} and reports no physical devices",
                   backend.InstanceApiVersion().major, backend.InstanceApiVersion().minor,
                   backend.InstanceApiVersion().patch);
        backend.Shutdown();
        return kSkipReturnCode;
    }

    MONARC_LOG(RenderGraphDeviceTest, Info,
               "instance {}.{}.{} | {} deduplicated adapter(s) | validation layer {} | debug "
               "messenger {}",
               backend.InstanceApiVersion().major, backend.InstanceApiVersion().minor,
               backend.InstanceApiVersion().patch, adapters.Size(),
               backend.ValidationLayerEnabled() ? "enabled" : "not enabled",
               backend.DebugMessengerInstalled() ? "installed" : "absent");
    for (Monarc::usize i = 0; i < adapters.Size(); ++i) {
        MONARC_LOG(RenderGraphDeviceTest, Info, "deduplicated [{}] {} | tier {}", i,
                   adapters[i].name, Monarc::RHI::ToString(adapters[i].tier));
    }

    g_backend  = &backend;
    g_adapters = &adapters;

    doctest::Context context;
    context.applyCommandLine(static_cast<int>(forwarded.Size()), forwarded.Data());
    const int failures = context.run();

    // Cleared before `backend` and `adapters` go out of scope, so a doctest reporter or an
    // at-exit handler cannot reach a destroyed object.
    g_adapters = nullptr;
    g_backend  = nullptr;
    backend.Shutdown();
    return failures;
}
