# hearth

A Vulkan device, resource and command-list layer. Dynamic rendering; no `VkRenderPass` or
`VkFramebuffer` objects anywhere.

hearth links no windowing library. The host supplies a presentation surface by implementing
`hearth::Surface`; header-only adapters for GLFW, SDL3 and Android are in `hearth/surface/`.

## Requirements

- A Vulkan 1.3 loader and device, or newer, with `dynamicRendering`, `synchronization2` and
  descriptor indexing
- The Vulkan loader (Arch: `vulkan-icd-loader`); headers, VMA and vk-bootstrap are vendored
- A C++23 compiler, premake5

## API version

At device creation hearth takes the lowest of what the loader offers, what its vendored
headers describe, and what the device supports. 1.3 is the floor. On 1.4 it additionally
enables the features the device reports, and each has a `Caps()` field: `hostImageCopy`,
`pushDescriptor`, `maintenance5`.

`Caps().apiVersion` is what was negotiated, and `Caps().AtLeast(1, 4)` reads it.
`DeviceDesc::minimumApiVersion` raises the floor — a packed version, so `VK_API_VERSION_1_4`
rather than `4`. `CreateDevice` returns null and logs the reason when the loader or the device
cannot meet it.

## Build

```sh
./scripts/Setup.sh
make config=debug -j4
```

## Using it

Add hearth as a submodule — with `--recursive`, since hearth vendors its own — and include its
projects. `Dependencies.lua` works out where hearth is and sets `HearthRoot`, `IncludeDir` and
`Library` from that:

```lua
include "vendor/hearth/Dependencies.lua"
include (HearthRoot .. "/vendor-build/VulkanDeps.lua")
include (HearthRoot .. "/Hearth")
```

Link `Hearth`, `VulkanDeps` and `%{Library.Vulkan}`, which is `vulkan-1` on Windows and
`vulkan` elsewhere. Include dirs: `%{IncludeDir.Hearth}` and
`%{IncludeDir.VulkanHeaders}` — the Vulkan headers are vendored rather than taken from the
system, and `hearth/Device.h` includes `<vulkan/vulkan.h>` through `Surface.h`. Add
`%{IncludeDir.VMA}` as well if you include `hearth/vulkan/Native.h`.

A device needs a surface. The adapters are header-only — include the one that matches your
window in one of your own translation units:

```cpp
#include "hearth/surface/Sdl3Surface.h"    // or GlfwSurface.h, AndroidSurface.h

hearth::Sdl3Surface surface{ sdlWindow };

hearth::DeviceDesc desc;
desc.surface = &surface;          // null for headless: no swapchain, everything else works
desc.appName = "my app";
auto device = hearth::CreateDevice(desc);
```

A frame:

```cpp
if (auto* cmd = device->BeginFrame()) {       // null = skip this frame
    cmd->BeginRenderPass({ .clearColor = { 0.1f, 0.1f, 0.12f, 1.0f } });
    cmd->BindPipeline(pipeline);
    cmd->BindBindGroup(bindGroup);
    cmd->BindVertexBuffer(0, vertices);
    cmd->Draw(3);
    cmd->EndRenderPass();
    device->EndFrame();
}
```

Work outside the frame loop goes through `SubmitImmediate`, which blocks until the GPU retires
it. A render pass recorded there must name a target; there is no acquired swapchain image to
default to.

```cpp
device->SubmitImmediate([&](hearth::CommandList& cmd) {
    cmd.BeginRenderPass({ .target = target.get() });
    // ...
    cmd.EndRenderPass();
});

std::vector<hearth::u8> pixels(target->Width() * target->Height()
                               * hearth::FormatSize(hearth::Format::RGBA8_UNORM));
target->ReadPixels(pixels.data(), pixels.size());
```

Calling `SubmitImmediate` while a frame is open aborts — it blocks on the GPU, which would
stall the frame being recorded.

## Textures and render targets

`TextureDesc::generateMipmaps` rebuilds the full chain by successive halving on every upload —
`Upload`, `UploadLayer` and `UploadRegion` alike, so a one-pixel region update rebuilds the
whole chain. Off by default, and warned about and ignored on a depth or storage texture, which
cannot have one. `MipLevels()` reports what the texture actually got.

`maxAnisotropy` needs mipmaps to mean anything, and is clamped to `Caps().maxAnisotropy`; a
device without anisotropic filtering reports 1.

`TextureKind::Texture2DArray` takes `layers`. A `Cube` is always 6 layers — set `layers` to 6
or leave it at 1. Both are filled a layer at a time with `UploadLayer`, whose `size` is the
bytes for **one** layer. Cube face order is +X, -X, +Y, -Y, +Z, -Z.

A render target takes a list of `colorFormats`; the pipeline drawing into it must declare the
same formats in the same order. More than `Caps().maxColorAttachments` of them aborts.
`ReadPixels(data, size, index)` selects one and takes `size` in **bytes** —
`width * height * FormatSize(colorFormats[index])`.

`samples` above 1 multisamples: hearth owns the multisampled images and resolves them into the
sampleable `ColorTexture(i)` as each pass ends. It is rounded down to a count the device
actually supports, with a warning when that changes it. The supported counts are not
contiguous — a device can offer 1, 4 and 8 and no 2 — so `Caps().SupportedSamples(n)` answers
what a given request will become, and `Caps().sampleCountMask` has the full set.

## Compute

`CreateComputePipeline` takes one shader — created with `ShaderStage::Compute`, or it aborts —
and its bindings. Binding it switches the bind point, so `BindBindGroup` and `SetPushConstants`
are the same calls as for graphics.

```cpp
auto pipeline = device->CreateComputePipeline({
    .compute = shader,
    .bindings = { { .binding = 0, .type = hearth::BindingType::StorageBuffer,
                    .stages = hearth::StageBit(hearth::ShaderStage::Compute) } },
    .pushConstantSize = sizeof(Push),
});

device->SubmitImmediate([&](hearth::CommandList& cmd) {
    cmd.BindPipeline(pipeline);
    cmd.BindBindGroup(group);
    cmd.SetPushConstants(&push, sizeof(push));
    cmd.Dispatch((count + 63) / 64);
});
```

`Dispatch` counts **workgroups**: a shader with `local_size_x = 64` over 1000 items dispatches
16 groups, and the shader itself has to discard the tail. A group count of 0 aborts, so guard
`count == 0` before the division above. Binding a compute pipeline inside a render pass aborts.

`MemoryBarrier` makes everything written before it visible to everything after — between a
dispatch and the draw that reads its output, this is the call. It is a full barrier: all
stages, all access types. `hearth/vulkan/Native.h` exposes the command buffer for hand-written
ones.

A texture bound as `BindingType::StorageTexture` must be created with `TextureUsage::Storage`,
and those stay in `VK_IMAGE_LAYOUT_GENERAL` for their whole life.

## Threads

Resource creation — `CreateBuffer`, `CreateTexture`, `CreateShader`, `CreatePipeline`,
`CreateBindGroup`, `CreateRenderTarget` — and the uploads those perform are safe to call from
several threads at once. The frame loop is not:
`BeginFrame`, the `CommandList` it returns, `EndFrame` and `SubmitImmediate` belong to one
thread.

`DeviceDesc::pipelineCachePath` keeps the driver's pipeline cache between runs; it is written
when the device is destroyed. An empty path keeps the cache in memory for the process only. A
stale or corrupt file is discarded, not fatal.

## Windows, sizes and surface loss

The host drives three calls. `OnWindowResize(width, height)` is the ordinary one. The other two
are Android, where the `ANativeWindow` a surface was built from is destroyed when the activity
backgrounds and a *different* one arrives on return:

```cpp
hearth::AndroidSurface surface;

// onNativeWindowDestroyed
device->OnSurfaceLost();
surface.Reset(nullptr);

// onNativeWindowCreated(ANativeWindow* window)
surface.Reset(window);
device->OnSurfaceRecreated(surface);
```

The device, the allocator and every texture already uploaded survive that round trip; only the
presentation chain is rebuilt.

Sizes passed to `OnWindowResize`, and those reported by `Surface::FramebufferSize`, are in
**pixels**, not logical units. On a HiDPI display those differ by the scale factor, and a
swapchain built at the logical size renders a quarter-resolution image stretched over the window.

`BeginFrame` returns null on a lost device, the same as it does for a minimized window, so poll
`DeviceLost()` to tell the two apart. Nothing recovers a lost device in-process.

## Native Vulkan handles

`hearth/vulkan/Native.h` exposes every handle hearth owns — `VkDevice`, the current
`VkCommandBuffer`, the `VmaAllocator`, the handle behind each resource.

`hearth::vk::CurrentCommandBuffer(*device)` inside a frame is the same buffer hearth is
recording into, in the same pass; outside `BeginFrame`/`EndFrame`, and inside
`SubmitImmediate`, it is `VK_NULL_HANDLE`. Transition an image behind hearth's back and the layout it tracks for
that texture is wrong until you set it back.

## Logging

The default sink writes warnings and errors to stderr, trace and info to stdout, prefixed
`[hearth <level>]`. `LogSink` is a plain function pointer, so a replacement must be captureless;
per-instance state goes through the `user` pointer passed to `SetLogSink`.

```cpp
hearth::SetLogSink([](hearth::LogLevel level, std::string_view message, void* user) {
    static_cast<MyLog*>(user)->Write(level, message);
}, &myLog);
```

`SetAbortHandler` does the same for the fatal path. The handler must not return; hearth aborts
the process itself if it does.
