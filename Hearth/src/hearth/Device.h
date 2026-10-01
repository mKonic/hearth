#pragma once

#include "hearth/CommandList.h"
#include "hearth/Surface.h"

#include <functional>
#include <string>
#include <vector>

namespace hearth {

    struct DeviceCaps {
        std::string deviceName;
        std::string driverInfo;
        // The core version actually negotiated: the lowest of what the loader offers, what
        // hearth's headers describe, and what the device supports. hearth targets the highest
        // available rather than a version fixed at build time, so a machine that gains a
        // newer loader gets it without hearth changing.
        u32 apiVersion = 0;
        bool discrete = false;
        // True under lavapipe/llvmpipe. Worth knowing: it is the difference between "this machine
        // has no GPU driver installed" and "this frame is slow", and a headless CI box is usually
        // the former by design.
        bool softwareRasterizer = false;
        u32 maxTextureSize = 0;
        // 1 when the device has no anisotropic filtering, or it was not enabled. Asking a
        // texture for more than this is clamped, not refused.
        f32 maxAnisotropy = 1.0f;
        // The most colour attachments one pipeline may write, and the highest sample count
        // both colour and depth support. Ask rather than assume: 8 attachments and 8 samples
        // are common, neither is guaranteed.
        u32 maxColorAttachments = 1;
        u32 maxSamples = 1;
        // Which sample counts this device actually supports, as a bitmask of the counts
        // themselves: bit 2 set means 2x is available. NOT contiguous -- lavapipe offers
        // 1, 4 and 8 and no 2 -- so "at most maxSamples" is not the same question as
        // "supported", and a target rounds down through this rather than through a power of
        // two. `SupportedSamples(n)` is the usual way to ask.
        u32 sampleCountMask = 1;

        // The highest supported count not above `wanted`. Always at least 1.
        u32 SupportedSamples(u32 wanted) const {
            for (u32 count = 64; count >= 1; count >>= 1)
                if (count <= wanted && (sampleCountMask & count)) return count;
            return 1;
        }
        // The largest array binding this device accepts. Ask for this rather than declaring a
        // constant -- it is a device property, and a renderer that hardcodes 32 either wastes the
        // hardware or fails to create a pipeline on something smaller.
        u32 maxTexturesPerBindGroup = 0;
        // Whether DepthBias::clamp is honoured.
        bool depthBiasClamp = false;
        u32 framesInFlight = 2;

        // How render passes are recorded. True: dynamic rendering, core in 1.3 or through
        // VK_KHR_dynamic_rendering on an older driver. False: classic VkRenderPass and
        // VkFramebuffer objects, which hearth builds and caches itself -- the path for Vulkan
        // 1.1 drivers that never got the extension. The CommandList API is the same on both.
        bool dynamicRendering = false;

        // Descriptor indexing: arrays of textures that may be left partly written and indexed
        // with a non-uniform value (`nonuniformEXT` in GLSL). A sprite batcher's texture array
        // needs it; a renderer with one texture per draw does not. Optional unless
        // DeviceDesc::requireDescriptorIndexing.
        bool descriptorIndexing = false;
        // Whether a texture array can be rewritten while it is bound (BindGroup::Update during
        // a frame that already used it). Implies descriptorIndexing.
        bool updateAfterBind = false;

        // Core features present beyond Vulkan 1.3. Reported so a consumer can branch on them;
        // hearth does not yet route any of its own work through them.
        bool hostImageCopy = false;
        bool pushDescriptor = false;
        bool maintenance5 = false;

        // Whether the validation layers are actually loaded -- not merely whether they were
        // asked for. They are a request, and a machine without them installed builds the
        // instance happily without them.
        bool validationActive = false;

        // `Caps().AtLeast(1, 4)` reads better at a call site than a packed comparison.
        bool AtLeast(u32 major, u32 minor) const {
            return apiVersion >= ((major << 22) | (minor << 12));
        }
    };

    class Swapchain {
    public:
        virtual ~Swapchain() = default;
        virtual u32 Width()  const = 0;
        virtual u32 Height() const = 0;
        virtual Format ColorFormat() const = 0;
        // What DeviceDesc::swapchainDepthFormat and swapchainSamples asked for, as granted.
        // A pipeline drawn into the swapchain declares these two.
        virtual Format DepthFormat() const = 0;
        virtual u32 Samples() const = 0;
    };

    // One frame read back from the swapchain, tightly packed in the swapchain's own format
    // (usually BGRA8_UNORM).
    struct FrameCapture {
        u32 width = 0, height = 0;
        Format format = Format::Undefined;
        std::vector<u8> pixels;
    };

    struct DeviceDesc {
        // Null = headless: no swapchain, no presentation. Everything else works, which is what a
        // test that renders one image and reads it back needs, and what a CI box can actually run.
        Surface* surface = nullptr;
        // Off in a shipped build: the layers cost real frame time and print to a console
        // nobody is watching. Defaults to on only where NDEBUG is absent.
#ifdef NDEBUG
        bool enableValidation = false;
#else
        bool enableValidation = true;
#endif

        // Raise hearth's 1.1 floor when your own code needs a newer core feature. A packed
        // Vulkan version, so VK_API_VERSION_1_3 rather than 3 or 13; 0 leaves hearth's floor
        // in place. CreateDevice fails with the reason when it cannot be met.
        u32 minimumApiVersion = 0;

        // Depth and multisampling for passes that render straight into the swapchain
        // (RenderPassDesc::target = nullptr). hearth owns the images, sizes them with the
        // window and resolves into the presented image as each pass ends, so a 3D scene
        // needs no offscreen target and composite pass of its own. Samples are rounded down
        // to what the device supports; Swapchain::Samples() says what was granted.
        Format swapchainDepthFormat = Format::Undefined;
        u32 swapchainSamples = 1;

        // Refuse devices without descriptor indexing (DeviceCaps::descriptorIndexing) instead
        // of running without it. For a renderer whose shaders index texture arrays.
        bool requireDescriptorIndexing = false;

        // Where to keep the pipeline cache between runs. Empty means in-memory only, which
        // still helps a process that builds several similar pipelines but starts cold every
        // launch. The file is written when the device is destroyed, and a stale or corrupt
        // one is discarded rather than trusted -- the driver validates its own header.
        std::string pipelineCachePath;
        bool vsync = true;
        std::string appName = "hearth";
        std::string engineName = "hearth";
    };

    // Thread safety: resource creation (CreateBuffer, CreateTexture, CreateShader,
    // CreatePipeline, CreateBindGroup, CreateRenderTarget) and the uploads those perform are
    // safe to call from several threads at once, which is what an asset loader wants. The
    // frame loop is not -- BeginFrame, the CommandList it returns, EndFrame and
    // SubmitImmediate belong to one thread.
    class Device {
    public:
        virtual ~Device() = default;

        virtual const DeviceCaps& Caps() const = 0;
        virtual Swapchain* GetSwapchain() = 0;

        // The format to build a pipeline against when it renders into the swapchain. Undefined
        // when headless.
        Format SwapchainFormat() {
            auto* sc = GetSwapchain();
            return sc ? sc->ColorFormat() : Format::Undefined;
        }

        virtual Ref<Buffer>       CreateBuffer(const BufferDesc&) = 0;
        virtual Ref<Texture>      CreateTexture(const TextureDesc&) = 0;
        // Convenience: a solid 1x1 texture in 0xAABBGGRR order. Every batcher needs one so that
        // untextured geometry can sample pure white and show only its vertex colour.
        Ref<Texture> CreateSolidTexture(u32 rgba);
        virtual Ref<Shader>       CreateShader(const ShaderDesc&) = 0;
        virtual Ref<Pipeline>     CreatePipeline(const PipelineDesc&) = 0;
        virtual Ref<Pipeline>     CreateComputePipeline(const ComputePipelineDesc&) = 0;
        virtual Ref<BindGroup>    CreateBindGroup(const Ref<Pipeline>&,
                                                  const std::vector<BindGroupEntry>&) = 0;
        virtual Ref<RenderTarget> CreateRenderTarget(const RenderTargetDesc&) = 0;

        // --- the frame loop -----------------------------------------------------------------
        // Returns null when the frame must be skipped: minimized, the swapchain is being rebuilt,
        // or the surface is gone. Record nothing in that case and do not call EndFrame.
        virtual CommandList* BeginFrame() = 0;
        virtual void EndFrame() = 0;
        // Which of the Caps().framesInFlight slots the open frame uses. BeginFrame waits for
        // that slot's previous frame to retire, so per-frame data indexed by this (a dynamic
        // vertex buffer, a uniform buffer) is safe to rewrite once BeginFrame returns non-null.
        virtual u32 FrameIndex() const = 0;
        virtual void WaitIdle() = 0;

        // Records and submits work outside the frame loop, blocking until the GPU retires it.
        // For offscreen rendering that is not part of a presented frame -- a thumbnail, a frame
        // grab for a clip, a test that draws one image and reads it back. A render pass recorded
        // here must name a target; there is no swapchain image to default to.
        virtual void SubmitImmediate(const std::function<void(CommandList&)>& record) = 0;

        // Copies the next presented frame back to the CPU and hands it to `done`, from inside
        // that frame's EndFrame. EndFrame then waits for the GPU to finish the frame: a hitch,
        // which is fine for a screenshot or a bug report and wrong for every frame. Nothing
        // happens on a headless device or a frame that is skipped.
        virtual void CaptureNextFrame(std::function<void(const FrameCapture&)> done) = 0;

        // --- surface lifecycle --------------------------------------------------------------
        // Resize is the easy one. The other two are Android, and they are why this trio is on the
        // interface rather than hidden: when an activity leaves the foreground the ANativeWindow
        // the surface was made from is destroyed, and Vulkan requires the swapchain to go first.
        // It comes back with a *different* window, so the surface and everything derived from it
        // must be rebuilt -- while the device, the allocator and every texture already uploaded
        // survive untouched. A desktop window keeps one surface for its whole life and never
        // calls these.
        virtual void OnWindowResize(u32 width, u32 height) = 0;
        virtual void OnSurfaceLost() = 0;
        virtual void OnSurfaceRecreated(Surface& surface) = 0;

        // True once the driver has reported the device gone: a reset, a hang, a suspend it did not
        // survive. Every handle this device handed out is invalid and nothing recovers in-process.
        // Poll it to say so once and exit, rather than spinning on a window that cannot draw.
        virtual bool DeviceLost() const = 0;
    };

    enum class DeviceErrorCode : u8 {
        None,
        LoaderTooOld,       // the Vulkan loader is older than 1.1
        InstanceFailed,     // no driver at all, or instance creation refused
        SurfaceFailed,      // the host's Surface could not make a VkSurfaceKHR
        NoSuitableDevice,   // drivers exist, none meets the requirements: see `missing`
        DeviceFailed,       // a device was chosen and then refused to be created
    };

    // Why CreateDevice returned null, in a form a caller can act on: tell the player their
    // phone is unsupported, retry with lighter requirements, or report it.
    struct DeviceError {
        DeviceErrorCode code = DeviceErrorCode::None;
        // One line, the same one that was logged.
        std::string message;
        // For NoSuitableDevice: what the closest device lacked, e.g. "Vulkan 1.3",
        // "descriptor indexing", "presentation to this surface".
        std::vector<std::string> missing;
    };

    const char* DeviceErrorName(DeviceErrorCode code);

    // Creates the device, or returns null after logging exactly why. Never partially succeeds.
    // `error`, when given, receives the reason.
    Scope<Device> CreateDevice(const DeviceDesc& desc, DeviceError* error = nullptr);

}
