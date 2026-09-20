#include "hearth/Device.h"

namespace hearth {

    Ref<Texture> Device::CreateSolidTexture(u32 rgba) {
        TextureDesc desc{};
        desc.width = desc.height = 1;
        desc.format = Format::RGBA8_UNORM;
        // Nearest: a 1x1 texture has nothing to interpolate, and asking for linear makes some
        // drivers take the filtered path anyway for no reason.
        desc.minFilter = desc.magFilter = Filter::Nearest;
        desc.debugName = "solid";

        auto texture = CreateTexture(desc);
        texture->Upload(&rgba, sizeof(rgba));
        return texture;
    }

}
