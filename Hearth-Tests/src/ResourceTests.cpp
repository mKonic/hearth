#include "Harness.h"

#include <cstring>
#include <vector>

namespace hearth::tests {

void RunResourceTests() {
    Section("buffers");

    Device& gpu = Gpu();

    {
        // A host-visible buffer is persistently mapped; writing through Map() and reading it
        // straight back is what a batcher does every frame.
        BufferDesc desc;
        desc.size      = 256;
        desc.usage     = BufferUsage::Vertex;
        desc.memory    = MemoryKind::HostVisible;
        desc.debugName = "host-visible";
        auto buffer = gpu.CreateBuffer(desc);

        CHECK(buffer->Size() == 256);
        CHECK(buffer->Map() != nullptr);

        const u32 pattern[4] = { 0xDEADBEEF, 0x12345678, 0, 0xFFFFFFFF };
        buffer->Upload(pattern, sizeof(pattern));
        CHECK(std::memcmp(buffer->Map(), pattern, sizeof(pattern)) == 0);

        // Uploading at an offset must not disturb what came before it.
        const u32 tail = 0xA5A5A5A5;
        buffer->Upload(&tail, sizeof(tail), 128);
        CHECK(std::memcmp(buffer->Map(), pattern, sizeof(pattern)) == 0);
        CHECK(std::memcmp(static_cast<u8*>(buffer->Map()) + 128, &tail, sizeof(tail)) == 0);
    }

    {
        // Device-local takes the staging path instead, and has no mapping to hand back.
        BufferDesc desc;
        desc.size      = 1024;
        desc.usage     = BufferUsage::Index;
        desc.memory    = MemoryKind::DeviceLocal;
        desc.debugName = "device-local";
        auto buffer = gpu.CreateBuffer(desc);

        CHECK(buffer->Size() == 1024);
        CHECK(buffer->Map() == nullptr);

        std::vector<u32> indices(256);
        for (u32 i = 0; i < indices.size(); ++i) indices[i] = i;
        buffer->Upload(indices.data(), indices.size() * sizeof(u32));   // must not crash
    }

    Section("textures");

    {
        TextureDesc desc;
        desc.width = desc.height = 4;
        desc.format    = Format::RGBA8_UNORM;
        desc.debugName = "checker";
        auto texture = gpu.CreateTexture(desc);

        CHECK(texture->Width() == 4);
        CHECK(texture->Height() == 4);
        CHECK_NEAR(texture->AspectRatio(), 1.0f, 0.001f);

        std::vector<u8> pixels(4 * 4 * 4, 0x80);
        texture->Upload(pixels.data(), pixels.size());

        // A sub-rectangle upload must stay inside the image.
        std::vector<u8> corner(2 * 2 * 4, 0xFF);
        texture->UploadRegion(corner.data(), 0, 0, 2, 2);
    }

    {
        // The 1x1 white texture every batcher needs for untextured geometry.
        auto white = gpu.CreateSolidTexture(0xFFFFFFFFu);
        CHECK(white->Width() == 1 && white->Height() == 1);
    }

    {
        // Non-square, to catch a width/height transposition in the upload path.
        TextureDesc desc;
        desc.width  = 8;
        desc.height = 2;
        desc.debugName = "wide";
        auto texture = gpu.CreateTexture(desc);
        CHECK(texture->Width() == 8 && texture->Height() == 2);
        CHECK_NEAR(texture->AspectRatio(), 4.0f, 0.001f);

        std::vector<u8> pixels(8 * 2 * 4, 0x40);
        texture->Upload(pixels.data(), pixels.size());
    }

    Section("render targets");

    {
        RenderTargetDesc desc;
        desc.width = 16;
        desc.height = 8;
        desc.colorFormats = { Format::RGBA8_UNORM };
        desc.debugName = "resizable";
        auto target = gpu.CreateRenderTarget(desc);

        CHECK(target->Width() == 16 && target->Height() == 8);
        CHECK(target->ColorTexture() != nullptr);
        CHECK(target->ColorTexture()->Width() == 16);

        target->Resize(32, 32);
        CHECK(target->Width() == 32 && target->Height() == 32);
        // The colour texture has to follow the target, not keep the old allocation.
        CHECK(target->ColorTexture()->Width() == 32);
        CHECK(target->ColorTexture()->Height() == 32);

        // Resizing to the same size is a no-op, and to zero is ignored rather than fatal.
        target->Resize(32, 32);
        target->Resize(0, 0);
        CHECK(target->Width() == 32);
    }
}

}
