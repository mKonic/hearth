#pragma once

#include "hearth/Base.h"

#include <string>
#include <vector>

// There is no Backend enum here.
//
// VAE, which this layer was extracted from, carried one with Vulkan/D3D12/Software arms, and two
// of the three were files that logged "not implemented" and returned null. Vulkan already runs on
// Linux, Windows and Android, and on macOS/iOS through MoltenVK, so a second backend buys a
// platform nobody is shipping to. If one is ever genuinely needed it is a design change with real
// decisions in it -- not an enum value that was reserved years earlier and predicted them wrong.

namespace hearth {

    enum class Format : u8 {
        Undefined = 0,
        R8_UNORM, RG8_UNORM, RGBA8_UNORM, RGBA8_SRGB, BGRA8_UNORM, BGRA8_SRGB,
        R16_SFLOAT, RG16_SFLOAT, RGBA16_SFLOAT,
        R32_SFLOAT, RG32_SFLOAT, RGB32_SFLOAT, RGBA32_SFLOAT,
        R32_UINT, RG32_UINT, RGBA32_UINT,
        D32_SFLOAT,
    };

    u32  FormatSize(Format f);
    bool IsDepthFormat(Format f);
    const char* FormatName(Format f);

    enum class BufferUsage : u8 { Vertex, Index, Uniform, Storage, Staging };

    // Where a buffer lives. HostVisible is written every frame by the CPU (instance data,
    // uniforms) and stays mapped; DeviceLocal is uploaded once through a staging copy.
    enum class MemoryKind : u8 { DeviceLocal, HostVisible };

    // Storage is a texture a compute shader writes into. It differs from the others in that
    // it lives in VK_IMAGE_LAYOUT_GENERAL rather than being transitioned per use -- a storage
    // image that is also sampled cannot be in two layouts at once.
    enum class TextureUsage : u8 { Sampled, RenderTarget, DepthTarget, Storage };

    enum class Filter : u8 { Nearest, Linear };
    enum class AddressMode : u8 { ClampToEdge, Repeat, MirroredRepeat, ClampToBorder };

    enum class ShaderStage : u8 { Vertex, Fragment, Compute };

    enum class PrimitiveTopology : u8 { TriangleList, TriangleStrip, LineList };

    // Straight-alpha "over" is the default. Premultiplied is what an offscreen layer composites
    // with, since compositing a faded layer with straight alpha double-darkens its edges.
    // Additive is the glow path: a sprite batcher can interleave it with alpha quads freely
    // because 2D draw order is submission order.
    enum class BlendMode : u8 { None, AlphaStraight, AlphaPremultiplied, Additive };

    enum class LoadOp : u8 { Load, Clear, DontCare };

    enum class BindingType : u8 { UniformBuffer, StorageBuffer, SampledTexture, StorageTexture };

    enum class IndexType : u8 { U16, U32 };

    struct VertexAttribute {
        u32 location = 0;
        Format format = Format::RG32_SFLOAT;
        u32 offset = 0;
    };

    enum class VertexStepMode : u8 { Vertex, Instance };

    struct VertexBufferLayout {
        u32 stride = 0;
        VertexStepMode stepMode = VertexStepMode::Vertex;
        std::vector<VertexAttribute> attributes;
    };

    struct BindingSlot {
        u32 binding = 0;
        BindingType type = BindingType::UniformBuffer;
        // >1 makes this an array binding -- the shape a sprite batcher needs to address many
        // textures from one draw. Pass DeviceCaps::maxTexturesPerBindGroup rather than a constant;
        // the limit is a property of the device, not of the renderer.
        u32 count = 1;
        u8  stages = 0;                // bitmask over ShaderStage, via StageBit
    };

    constexpr u8 StageBit(ShaderStage s) { return static_cast<u8>(1u << static_cast<u8>(s)); }

    struct Viewport {
        f32 x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
        f32 minDepth = 0.0f, maxDepth = 1.0f;
    };

    struct ScissorRect { i32 x = 0, y = 0; u32 width = 0, height = 0; };

}
