#include "Harness.h"
#include "Shaders.h"

#include <cmath>
#include <vector>

namespace hearth {
namespace tests {

namespace {

struct FlatVertex { f32 x, y, r, g, b, a; };
struct FlatPush   { f32 offsetX = 0, offsetY = 0, scaleX = 1, scaleY = 1; };

Ref<Buffer> Quad(f32 r, f32 g, f32 b, f32 a) {
    const FlatVertex v[6] = {
        { -1, -1, r, g, b, a }, {  1, -1, r, g, b, a }, {  1,  1, r, g, b, a },
        {  1,  1, r, g, b, a }, { -1,  1, r, g, b, a }, { -1, -1, r, g, b, a },
    };
    auto buffer = Gpu().CreateBuffer(BufferDesc{
        .size = sizeof(v), .usage = BufferUsage::Vertex,
        .memory = MemoryKind::HostVisible, .debugName = "feature-quad" });
    buffer->Upload(v, sizeof(v));
    return buffer;
}

VertexBufferLayout FlatLayout() {
    return VertexBufferLayout{
        .stride = sizeof(FlatVertex),
        .attributes = { { 0, Format::RG32_SFLOAT, 0 }, { 1, Format::RGBA32_SFLOAT, 8 } } };
}

} // namespace

void RunFeatureTests() {
    Device& gpu = Gpu();

    Section("mipmaps");

    {
        // A 64x64 texture with a full chain is 7 levels: 64,32,16,8,4,2,1.
        TextureDesc desc;
        desc.width = desc.height = 64;
        desc.generateMipmaps = true;
        desc.debugName = "mipped";
        auto texture = gpu.CreateTexture(desc);
        CHECK_MSG(texture->MipLevels() == 7, "64x64 gave {} mip levels, expected 7",
                  texture->MipLevels());

        std::vector<u8> pixels(64 * 64 * 4, 0x80);
        texture->Upload(pixels.data(), pixels.size());   // builds the chain

        // Non-square: 64x16 halves to 1x1 in 7 steps as well, since the chain runs until
        // BOTH dimensions reach 1.
        TextureDesc wide = desc;
        wide.width = 64; wide.height = 16; wide.debugName = "mipped-wide";
        auto wideTexture = gpu.CreateTexture(wide);
        CHECK_MSG(wideTexture->MipLevels() == 7, "64x16 gave {} levels, expected 7",
                  wideTexture->MipLevels());
        std::vector<u8> widePixels(64 * 16 * 4, 0x40);
        wideTexture->Upload(widePixels.data(), widePixels.size());

        // Off by default, so nothing that did not ask starts paying for a chain.
        TextureDesc plain;
        plain.width = plain.height = 64;
        plain.debugName = "unmipped";
        CHECK(gpu.CreateTexture(plain)->MipLevels() == 1);
    }

    Section("anisotropy");

    {
        // Requesting more than the device offers is clamped, not refused -- so this has to
        // produce a working texture whatever the hardware is.
        TextureDesc desc;
        desc.width = desc.height = 32;
        desc.generateMipmaps = true;
        desc.maxAnisotropy = 16.0f;
        desc.debugName = "aniso";
        auto texture = gpu.CreateTexture(desc);
        CHECK(texture != nullptr);
        std::vector<u8> pixels(32 * 32 * 4, 0xC0);
        texture->Upload(pixels.data(), pixels.size());

        CHECK_MSG(gpu.Caps().maxAnisotropy >= 1.0f, "maxAnisotropy reported as {}",
                  gpu.Caps().maxAnisotropy);
    }

    Section("array and cube textures");

    {
        TextureDesc desc;
        desc.width = desc.height = 8;
        desc.kind   = TextureKind::Texture2DArray;
        desc.layers = 4;
        desc.debugName = "array";
        auto texture = gpu.CreateTexture(desc);

        // Each layer gets a different value, which is what would expose a layer index that
        // was ignored and wrote everything into layer 0.
        for (u32 layer = 0; layer < 4; ++layer) {
            std::vector<u8> pixels(8 * 8 * 4, static_cast<u8>(0x40 * (layer + 1)));
            texture->UploadLayer(pixels.data(), pixels.size(), layer);
        }
        CHECK(texture->Width() == 8);
    }

    {
        TextureDesc desc;
        desc.width = desc.height = 8;
        desc.kind   = TextureKind::Cube;
        desc.layers = 6;
        desc.debugName = "cube";
        auto cube = gpu.CreateTexture(desc);
        for (u32 face = 0; face < 6; ++face) {
            std::vector<u8> pixels(8 * 8 * 4, static_cast<u8>(0x20 * (face + 1)));
            cube->UploadLayer(pixels.data(), pixels.size(), face);
        }
        CHECK(cube != nullptr);
    }

    Section("multisampling");

    {
        CHECK_MSG(gpu.Caps().maxSamples >= 1, "maxSamples reported as {}", gpu.Caps().maxSamples);

        const u32 samples = std::min(4u, gpu.Caps().maxSamples);

        PipelineDesc desc;
        desc.vertex = gpu.CreateShader(ShaderDesc{
            .spirv = std::vector<u32>(std::begin(kFlatVert), std::end(kFlatVert)),
            .stage = ShaderStage::Vertex, .debugName = "msaa.vert" });
        desc.fragment = gpu.CreateShader(ShaderDesc{
            .spirv = std::vector<u32>(std::begin(kFlatFrag), std::end(kFlatFrag)),
            .stage = ShaderStage::Fragment, .debugName = "msaa.frag" });
        desc.vertexBuffers = { FlatLayout() };
        desc.pushConstantSize = sizeof(FlatPush);
        desc.blend = BlendMode::None;
        desc.colorFormats = { Format::RGBA8_UNORM };
        desc.samples = samples;
        desc.debugName = "msaa";
        auto pipeline = gpu.CreatePipeline(desc);

        RenderTargetDesc targetDesc;
        targetDesc.width = targetDesc.height = 16;
        targetDesc.colorFormats = { Format::RGBA8_UNORM };
        targetDesc.samples = samples;
        targetDesc.debugName = "msaa-target";
        auto target = gpu.CreateRenderTarget(targetDesc);

        // The readback comes from the resolve image, so a full-coverage quad must land as a
        // flat colour -- if resolve were skipped this would read as the clear.
        auto quad = Quad(0.0f, 0.0f, 1.0f, 1.0f);
        gpu.SubmitImmediate([&](CommandList& cmd) {
            cmd.BeginRenderPass(RenderPassDesc{
                .target = target.get(), .loadOp = LoadOp::Clear,
                .clearColor = { 1.0f, 0.0f, 0.0f, 1.0f } });
            cmd.BindPipeline(pipeline);
            cmd.BindVertexBuffer(0, quad);
            const FlatPush push;
            cmd.SetPushConstants(&push, sizeof(push));
            cmd.Draw(6);
            cmd.EndRenderPass();
        });

        std::vector<u8> pixels(16 * 16 * 4);
        target->ReadPixels(pixels.data(), pixels.size());
        const u8* centre = &pixels[(8 * 16 + 8) * 4];
        CHECK_MSG(centre[0] < 4 && centre[2] > 250,
                  "{}x MSAA resolve gave ({},{},{}), expected blue",
                  samples, centre[0], centre[1], centre[2]);

        // A sample count above what the device supports is clamped, not fatal.
        RenderTargetDesc absurd = targetDesc;
        absurd.samples = 64;
        absurd.debugName = "msaa-clamped";
        auto clamped = gpu.CreateRenderTarget(absurd);
        CHECK(clamped != nullptr);
    }

    Section("multiple render targets");

    {
        CHECK(gpu.Caps().maxColorAttachments >= 2);

        PipelineDesc desc;
        desc.vertex = gpu.CreateShader(ShaderDesc{
            .spirv = std::vector<u32>(std::begin(kFlatVert), std::end(kFlatVert)),
            .stage = ShaderStage::Vertex, .debugName = "mrt.vert" });
        desc.fragment = gpu.CreateShader(ShaderDesc{
            .spirv = std::vector<u32>(std::begin(kMrtFrag), std::end(kMrtFrag)),
            .stage = ShaderStage::Fragment, .debugName = "mrt.frag" });
        desc.vertexBuffers = { FlatLayout() };
        desc.pushConstantSize = sizeof(FlatPush);
        desc.blend = BlendMode::None;
        desc.colorFormats = { Format::RGBA8_UNORM, Format::RGBA8_UNORM };
        desc.debugName = "mrt";
        auto pipeline = gpu.CreatePipeline(desc);

        RenderTargetDesc targetDesc;
        targetDesc.width = targetDesc.height = 8;
        targetDesc.colorFormats = { Format::RGBA8_UNORM, Format::RGBA8_UNORM };
        targetDesc.debugName = "mrt-target";
        auto target = gpu.CreateRenderTarget(targetDesc);
        CHECK(target->ColorAttachmentCount() == 2);

        // Red in, so attachment 0 is red and attachment 1 is the channel-swapped blue.
        auto quad = Quad(1.0f, 0.0f, 0.0f, 1.0f);
        gpu.SubmitImmediate([&](CommandList& cmd) {
            cmd.BeginRenderPass(RenderPassDesc{
                .target = target.get(), .loadOp = LoadOp::Clear,
                .clearColor = { 0.0f, 1.0f, 0.0f, 1.0f } });
            cmd.BindPipeline(pipeline);
            cmd.BindVertexBuffer(0, quad);
            const FlatPush push;
            cmd.SetPushConstants(&push, sizeof(push));
            cmd.Draw(6);
            cmd.EndRenderPass();
        });

        std::vector<u8> first(8 * 8 * 4), second(8 * 8 * 4);
        target->ReadPixels(first.data(), first.size(), 0);
        target->ReadPixels(second.data(), second.size(), 1);

        const u8* a = &first[(4 * 8 + 4) * 4];
        const u8* b = &second[(4 * 8 + 4) * 4];
        CHECK_MSG(a[0] > 250 && a[2] < 4, "attachment 0 gave ({},{},{}), expected red",
                  a[0], a[1], a[2]);
        CHECK_MSG(b[0] < 4 && b[2] > 250, "attachment 1 gave ({},{},{}), expected blue",
                  b[0], b[1], b[2]);
    }
}

} // namespace tests
} // namespace hearth
