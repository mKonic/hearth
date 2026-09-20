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

Link `Hearth`, `VulkanDeps` and `%{Library.Vulkan}`. Only the Linux build is exercised;
`Dependencies.lua` picks the Windows loader name but nothing else there has been tried. Include dirs: `%{IncludeDir.Hearth}` and
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
                               * hearth::FormatSize(format));
target->ReadPixels(pixels.data(), pixels.size());
```

`ReadPixels` takes a size in **bytes** and writes the target's own `colorFormat`. Ask for an
RGBA target and no channel swizzling is needed. Calling `SubmitImmediate` while a frame is
open aborts — it blocks on the GPU, which would stall the frame being recorded.

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
`DeviceLost()` to tell the two apart. Nothing recovers a lost device in-process; report it and
exit rather than spinning on a window that cannot draw.

## Native Vulkan handles

`hearth/vulkan/Native.h` exposes every handle hearth owns — `VkDevice`, the current
`VkCommandBuffer`, the `VmaAllocator`, the handle behind each resource.

`hearth::vk::CurrentCommandBuffer(device)` inside a frame is the same buffer hearth is
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
