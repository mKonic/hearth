#include "Harness.h"

namespace hearth::tests {

void RunDeviceTests() {
    Section("device");

    Device& gpu = Gpu();
    const DeviceCaps& caps = gpu.Caps();

    CHECK(!caps.deviceName.empty());

    // hearth's floor. A device below it should never have been selected.
    CHECK_MSG(caps.AtLeast(1, 3), "negotiated {}.{}, below hearth's 1.3 floor",
              VK_API_VERSION_MAJOR(caps.apiVersion), VK_API_VERSION_MINOR(caps.apiVersion));

    // AtLeast must be monotonic, and must not claim a version above the one negotiated.
    CHECK(caps.AtLeast(1, 0));
    CHECK(caps.AtLeast(1, 3));
    if (!caps.AtLeast(1, 4)) CHECK(!caps.hostImageCopy && !caps.pushDescriptor);

    // Descriptor-array sizing comes from the device, so a renderer asking for
    // maxTexturesPerBindGroup slots must actually get a pipeline.
    CHECK(caps.maxTexturesPerBindGroup > 0);
    CHECK(caps.maxTextureSize >= 4096);
    CHECK(caps.framesInFlight >= 1);

    // Headless means no swapchain, and SwapchainFormat has to say so rather than hand back
    // a format nothing can render into.
    CHECK(gpu.GetSwapchain() == nullptr);
    CHECK(gpu.SwapchainFormat() == Format::Undefined);

    CHECK(!gpu.DeviceLost());

    // Format tables have to agree with themselves in both directions.
    CHECK(FormatSize(Format::RGBA8_UNORM) == 4);
    CHECK(FormatSize(Format::R8_UNORM) == 1);
    CHECK(FormatSize(Format::RGBA32_SFLOAT) == 16);
    CHECK(FormatSize(Format::Undefined) == 0);
    CHECK(IsDepthFormat(Format::D32_SFLOAT));
    CHECK(!IsDepthFormat(Format::RGBA8_UNORM));
}

}
