#include "Harness.h"
#include "Shaders.h"

#include <vector>

namespace hearth::tests {

void RunComputeTests() {
    Section("compute dispatch");

    Device& gpu = Gpu();

    {
        // A compute shader filling a host-visible storage buffer with index * multiplier.
        // The CPU can reproduce the arithmetic exactly, so a failure is a wrong number rather
        // than something that merely looks off.
        constexpr u32 kCount = 1000;
        constexpr u32 kMultiplier = 7;
        constexpr u32 kLocalSize = 64;      // must match fill.comp

        auto shader = gpu.CreateShader(ShaderDesc{
            .spirv = std::vector<u32>(std::begin(kFillComp), std::end(kFillComp)),
            .stage = ShaderStage::Compute, .debugName = "fill.comp" });
        CHECK(shader->Stage() == ShaderStage::Compute);

        struct Push { u32 count; u32 multiplier; };

        auto pipeline = gpu.CreateComputePipeline(ComputePipelineDesc{
            .compute = shader,
            .bindings = { BindingSlot{ .binding = 0, .type = BindingType::StorageBuffer,
                                       .stages = StageBit(ShaderStage::Compute) } },
            .pushConstantSize = sizeof(Push),
            .debugName = "fill" });
        CHECK(pipeline != nullptr);
        CHECK(pipeline->IsCompute());

        auto buffer = gpu.CreateBuffer(BufferDesc{
            .size = kCount * sizeof(u32), .usage = BufferUsage::Storage,
            .memory = MemoryKind::HostVisible, .debugName = "compute-out" });

        // Poison it, so a dispatch that silently does nothing fails instead of passing on
        // memory that happened to be zero.
        auto* mapped = static_cast<u32*>(buffer->Map());
        for (u32 i = 0; i < kCount; ++i) mapped[i] = 0xDEADBEEF;

        auto group = gpu.CreateBindGroup(pipeline, { BindGroupEntry{
            .binding = 0, .type = BindingType::StorageBuffer, .buffer = buffer } });

        const Push push{ kCount, kMultiplier };
        gpu.SubmitImmediate([&](CommandList& cmd) {
            cmd.BindPipeline(pipeline);
            cmd.BindBindGroup(group);
            cmd.SetPushConstants(&push, sizeof(push));
            // Workgroups, not invocations: 1000 items at 64 per group is 16 groups, and the
            // shader itself discards the 24-item tail of the last one.
            cmd.Dispatch((kCount + kLocalSize - 1) / kLocalSize);
            cmd.MemoryBarrier();
        });

        bool allCorrect = true;
        u32 firstBad = 0;
        for (u32 i = 0; i < kCount; ++i) {
            if (mapped[i] != i * kMultiplier) { allCorrect = false; firstBad = i; break; }
        }
        CHECK_MSG(allCorrect, "compute output wrong at index {}: got {}, expected {}",
                  firstBad, mapped[firstBad], firstBad * kMultiplier);

        // The element one past the dispatched range must be untouched -- this is what catches
        // a workgroup count computed with a round-down instead of a round-up.
        auto tail = gpu.CreateBuffer(BufferDesc{
            .size = 256 * sizeof(u32), .usage = BufferUsage::Storage,
            .memory = MemoryKind::HostVisible, .debugName = "compute-tail" });
        auto* tailMapped = static_cast<u32*>(tail->Map());
        for (u32 i = 0; i < 256; ++i) tailMapped[i] = 0xABCDEF01;

        auto tailGroup = gpu.CreateBindGroup(pipeline, { BindGroupEntry{
            .binding = 0, .type = BindingType::StorageBuffer, .buffer = tail } });
        const Push tailPush{ 100, 1 };      // only the first 100 of 256 may be written
        gpu.SubmitImmediate([&](CommandList& cmd) {
            cmd.BindPipeline(pipeline);
            cmd.BindBindGroup(tailGroup);
            cmd.SetPushConstants(&tailPush, sizeof(tailPush));
            cmd.Dispatch(2);                // 128 invocations over a 100-item bound
            cmd.MemoryBarrier();
        });
        CHECK(tailMapped[99] == 99);
        CHECK_MSG(tailMapped[100] == 0xABCDEF01,
                  "the shader wrote past its own bound: index 100 is {}", tailMapped[100]);
    }

    Section("storage images");

    {
        // Compute paints a storage image; the ordinary graphics path then samples it. This is
        // the handoff that needs both the GENERAL layout and a barrier between the two.
        constexpr u32 kSize = 16;

        auto shader = gpu.CreateShader(ShaderDesc{
            .spirv = std::vector<u32>(std::begin(kPaintComp), std::end(kPaintComp)),
            .stage = ShaderStage::Compute, .debugName = "paint.comp" });

        struct Push { f32 r, g, b, a; };

        auto pipeline = gpu.CreateComputePipeline(ComputePipelineDesc{
            .compute = shader,
            .bindings = { BindingSlot{ .binding = 0, .type = BindingType::StorageTexture,
                                       .stages = StageBit(ShaderStage::Compute) } },
            .pushConstantSize = sizeof(Push),
            .debugName = "paint" });

        TextureDesc desc;
        desc.width = desc.height = kSize;
        desc.format = Format::RGBA8_UNORM;
        desc.usage  = TextureUsage::Storage;
        desc.debugName = "painted";
        auto texture = gpu.CreateTexture(desc);

        auto group = gpu.CreateBindGroup(pipeline, { BindGroupEntry{
            .binding = 0, .type = BindingType::StorageTexture, .textures = { texture } } });

        const Push push{ 0.0f, 1.0f, 1.0f, 1.0f };   // cyan
        gpu.SubmitImmediate([&](CommandList& cmd) {
            cmd.BindPipeline(pipeline);
            cmd.BindBindGroup(group);
            cmd.SetPushConstants(&push, sizeof(push));
            cmd.Dispatch(kSize / 8, kSize / 8);      // local_size is 8x8
            cmd.MemoryBarrier();
        });

        // Read it back by sampling it through the textured pipeline, which is the path a real
        // consumer would take and which also proves the sampled-in-GENERAL descriptor is
        // written correctly.
        PipelineDesc gfx;
        gfx.vertex = gpu.CreateShader(ShaderDesc{
            .spirv = std::vector<u32>(std::begin(kTexturedVert), std::end(kTexturedVert)),
            .stage = ShaderStage::Vertex, .debugName = "sample.vert" });
        gfx.fragment = gpu.CreateShader(ShaderDesc{
            .spirv = std::vector<u32>(std::begin(kTexturedFrag), std::end(kTexturedFrag)),
            .stage = ShaderStage::Fragment, .debugName = "sample.frag" });
        struct TexVertex { f32 x, y, u, v; };
        gfx.vertexBuffers = { VertexBufferLayout{
            .stride = sizeof(TexVertex),
            .attributes = { { 0, Format::RG32_SFLOAT, 0 }, { 1, Format::RG32_SFLOAT, 8 } } } };
        gfx.bindings = { BindingSlot{ .binding = 0, .type = BindingType::SampledTexture,
                                      .count = 4, .stages = StageBit(ShaderStage::Fragment) } };
        gfx.pushConstantSize = sizeof(u32);
        gfx.blend = BlendMode::None;
        gfx.colorFormats = { Format::RGBA8_UNORM };
        gfx.debugName = "sample-storage";
        auto gfxPipeline = gpu.CreatePipeline(gfx);
        auto gfxGroup = gpu.CreateBindGroup(gfxPipeline, { BindGroupEntry{
            .binding = 0, .type = BindingType::SampledTexture, .textures = { texture } } });

        const TexVertex quad[6] = {
            { -1, -1, 0, 0 }, { 1, -1, 1, 0 }, { 1, 1, 1, 1 },
            {  1,  1, 1, 1 }, { -1, 1, 0, 1 }, { -1, -1, 0, 0 },
        };
        auto vertices = gpu.CreateBuffer(BufferDesc{
            .size = sizeof(quad), .usage = BufferUsage::Vertex,
            .memory = MemoryKind::HostVisible, .debugName = "sample-quad" });
        vertices->Upload(quad, sizeof(quad));

        const u32 slot = 0;
        auto pixels = RenderToPixels(4, 4, Color{ 1.0f, 0.0f, 0.0f, 1.0f },
                                     [&](CommandList& cmd) {
            cmd.BindPipeline(gfxPipeline);
            cmd.BindBindGroup(gfxGroup);
            cmd.BindVertexBuffer(0, vertices);
            cmd.SetPushConstants(&slot, sizeof(slot));
            cmd.Draw(6);
        });
        const Pixel sampled = At(pixels, 4, 2, 2);
        CHECK_MSG(sampled.r < 8 && sampled.g > 247 && sampled.b > 247,
                  "compute-painted image sampled as ({},{},{}), expected cyan",
                  sampled.r, sampled.g, sampled.b);
    }
}

}
