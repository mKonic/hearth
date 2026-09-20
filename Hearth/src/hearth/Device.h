#pragma once

#include "hearth/CommandList.h"
#include "hearth/Surface.h"

#include <functional>
#include <string>

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
        u32 framesInFlight = 2;

        // Core features present beyond hearth's 1.3 floor. Reported so a consumer can branch
        // on them; hearth does not yet route any of its own work through them.
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

        // Raise hearth's 1.3 floor when your own code needs a newer core feature. A packed
        // Vulkan version, so VK_API_VERSION_1_4 rather than 4 or 14; 0 leaves hearth's floor
        // in place. CreateDevice returns null and logs the reason when it cannot be met.
        u32 minimumApiVersion = 0;

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
        virtual void WaitIdle() = 0;

        // Records and submits work outside the frame loop, blocking until the GPU retires it.
        // For offscreen rendering that is not part of a presented frame -- a thumbnail, a frame
        // grab for a clip, a test that draws one image and reads it back. A render pass recorded
        // here must name a target; there is no swapchain image to default to.
        virtual void SubmitImmediate(const std::function<void(CommandList&)>& record) = 0;

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

    // Creates the device, or returns null after logging exactly why. Never partially succeeds.
    Scope<Device> CreateDevice(const DeviceDesc& desc);

}
