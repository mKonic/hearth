#pragma once

#include "Check.h"

#include <hearth/Device.h>
#include <hearth/Log.h>

#include <functional>
#include <vector>

namespace hearth::tests {

    // Every test shares one headless device. Creating one per test would spend most of the
    // suite's runtime in driver initialisation, and nothing here mutates device state in a
    // way another test can observe.
    Device& Gpu();

    // An RGBA8 pixel read back from a render target.
    struct Pixel {
        u8 r = 0, g = 0, b = 0, a = 0;
    };

    // Renders `record` into a fresh target of this size and reads it back.
    std::vector<Pixel> RenderToPixels(u32 width, u32 height, Color clear,
                                      const std::function<void(CommandList&)>& record);

    inline Pixel At(const std::vector<Pixel>& pixels, u32 width, u32 x, u32 y) {
        return pixels[y * width + x];
    }

    void RunDeviceTests();
    void RunResourceTests();
    void RunRenderTests();
    void RunSystemTests();
    void RunComputeTests();
    void RunFeatureTests();

}
