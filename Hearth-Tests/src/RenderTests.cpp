#include "Harness.h"
#include "Shaders.h"

#include <cstring>
#include <vector>

namespace hearth::tests {

namespace {

struct FlatVertex {
    f32 x, y;
    f32 r, g, b, a;
};

struct TexturedVertex {
    f32 x, y;
    f32 u, v;
};

struct FlatPush {
    f32 offsetX = 0.0f, offsetY = 0.0f;
    f32 scaleX  = 1.0f, scaleY  = 1.0f;
};

Ref<Shader> FlatVert() {
    static Ref<Shader> shader = Gpu().CreateShader(ShaderDesc{
        .spirv = std::vector<u32>(std::begin(kFlatVert), std::end(kFlatVert)),
        .stage = ShaderStage::Vertex,
        .debugName = "flat.vert",
    });
    return shader;
}

Ref<Shader> FlatFrag() {
    static Ref<Shader> shader = Gpu().CreateShader(ShaderDesc{
        .spirv = std::vector<u32>(std::begin(kFlatFrag), std::end(kFlatFrag)),
        .stage = ShaderStage::Fragment,
        .debugName = "flat.frag",
    });
    return shader;
}

Ref<Pipeline> FlatPipeline(BlendMode blend) {
    PipelineDesc desc;
    desc.vertex   = FlatVert();
    desc.fragment = FlatFrag();
    desc.vertexBuffers = { VertexBufferLayout{
        .stride = sizeof(FlatVertex),
        .attributes = {
            { 0, Format::RG32_SFLOAT,   offsetof(FlatVertex, x) },
            { 1, Format::RGBA32_SFLOAT, offsetof(FlatVertex, r) },
        },
    } };
    desc.pushConstantSize = sizeof(FlatPush);
    desc.blend       = blend;
    desc.colorFormat = Format::RGBA8_UNORM;
    desc.debugName   = "flat";
    return Gpu().CreatePipeline(desc);
}

// Two triangles covering the whole target, in one colour.
Ref<Buffer> FullQuad(f32 r, f32 g, f32 b, f32 a) {
    const FlatVertex vertices[6] = {
        { -1.0f, -1.0f, r, g, b, a }, {  1.0f, -1.0f, r, g, b, a }, {  1.0f,  1.0f, r, g, b, a },
        {  1.0f,  1.0f, r, g, b, a }, { -1.0f,  1.0f, r, g, b, a }, { -1.0f, -1.0f, r, g, b, a },
    };
    auto buffer = Gpu().CreateBuffer(BufferDesc{
        .size = sizeof(vertices), .usage = BufferUsage::Vertex,
        .memory = MemoryKind::HostVisible, .debugName = "quad",
    });
    buffer->Upload(vertices, sizeof(vertices));
    return buffer;
}

int Diff(u8 actual, int expected) { return int(actual) - expected; }

void CheckPixel(const Pixel& pixel, int r, int g, int b, int a, int tolerance,
                const char* what) {
    const bool ok = std::abs(Diff(pixel.r, r)) <= tolerance
                 && std::abs(Diff(pixel.g, g)) <= tolerance
                 && std::abs(Diff(pixel.b, b)) <= tolerance
                 && std::abs(Diff(pixel.a, a)) <= tolerance;
    Record(ok, __FILE__, __LINE__,
           std::format("{}: got ({},{},{},{}), expected ({},{},{},{}) +/- {}", what,
                       pixel.r, pixel.g, pixel.b, pixel.a, r, g, b, a, tolerance));
}

} // namespace

void RunRenderTests() {
    Section("clear");

    {
        // The clear colour alone, with nothing drawn. If this is wrong nothing below means
        // anything.
        auto pixels = RenderToPixels(8, 8, Color{ 1.0f, 0.0f, 0.0f, 1.0f }, nullptr);
        CHECK(pixels.size() == 64);
        CheckPixel(At(pixels, 8, 0, 0), 255, 0, 0, 255, 1, "clear red, top-left");
        CheckPixel(At(pixels, 8, 7, 7), 255, 0, 0, 255, 1, "clear red, bottom-right");
    }

    {
        auto pixels = RenderToPixels(4, 4, Color{ 0.0f, 0.0f, 0.0f, 0.0f }, nullptr);
        CheckPixel(At(pixels, 4, 2, 2), 0, 0, 0, 0, 1, "clear transparent black");
    }

    Section("draw");

    {
        // A full-target green quad over a red clear: every pixel must be green, which proves
        // the vertex buffer, the pipeline and the render pass all line up.
        auto pipeline = FlatPipeline(BlendMode::None);
        auto quad = FullQuad(0.0f, 1.0f, 0.0f, 1.0f);

        auto pixels = RenderToPixels(8, 8, Color{ 1.0f, 0.0f, 0.0f, 1.0f },
                                     [&](CommandList& cmd) {
            cmd.BindPipeline(pipeline);
            cmd.BindVertexBuffer(0, quad);
            const FlatPush push;
            cmd.SetPushConstants(&push, sizeof(push));
            cmd.Draw(6);
        });
        CheckPixel(At(pixels, 8, 4, 4), 0, 255, 0, 255, 1, "full quad centre");
        CheckPixel(At(pixels, 8, 0, 0), 0, 255, 0, 255, 1, "full quad corner");
    }

    {
        // Half-scale quad: the centre is covered, the corners are not. This is the check that
        // would catch a viewport or clip-space mistake, and it pins hearth's documented
        // choice not to flip Y -- a vertex at y = -1 is the TOP of the image.
        auto pipeline = FlatPipeline(BlendMode::None);
        auto quad = FullQuad(0.0f, 0.0f, 1.0f, 1.0f);

        FlatPush push;
        push.scaleX = push.scaleY = 0.5f;
        // Push the quad up: -Y is up in Vulkan clip space.
        push.offsetY = -0.5f;

        auto pixels = RenderToPixels(16, 16, Color{ 0.0f, 0.0f, 0.0f, 1.0f },
                                     [&](CommandList& cmd) {
            cmd.BindPipeline(pipeline);
            cmd.BindVertexBuffer(0, quad);
            cmd.SetPushConstants(&push, sizeof(push));
            cmd.Draw(6);
        });
        // Quad now spans clip y in [-1, 0] => the TOP half of the image.
        CheckPixel(At(pixels, 16, 8, 4),  0, 0, 255, 255, 1, "offset quad lands in the top half");
        CheckPixel(At(pixels, 16, 8, 14), 0, 0, 0,   255, 1, "bottom half untouched");
    }

    Section("blending");

    {
        // Premultiplied source over an opaque background. src=(0,0.5,0) a=0.5 over red:
        // out = src + dst * (1 - a) = (0.5, 0.5, 0).
        auto pipeline = FlatPipeline(BlendMode::AlphaPremultiplied);
        auto quad = FullQuad(0.0f, 0.5f, 0.0f, 0.5f);

        auto pixels = RenderToPixels(4, 4, Color{ 1.0f, 0.0f, 0.0f, 1.0f },
                                     [&](CommandList& cmd) {
            cmd.BindPipeline(pipeline);
            cmd.BindVertexBuffer(0, quad);
            const FlatPush push;
            cmd.SetPushConstants(&push, sizeof(push));
            cmd.Draw(6);
        });
        CheckPixel(At(pixels, 4, 2, 2), 128, 128, 0, 255, 2, "premultiplied over red");
    }

    {
        // Straight alpha: out = src * a + dst * (1 - a). Green at a=0.5 over red => (0.5,0.25,0)
        auto pipeline = FlatPipeline(BlendMode::AlphaStraight);
        auto quad = FullQuad(0.0f, 0.5f, 0.0f, 0.5f);

        auto pixels = RenderToPixels(4, 4, Color{ 1.0f, 0.0f, 0.0f, 1.0f },
                                     [&](CommandList& cmd) {
            cmd.BindPipeline(pipeline);
            cmd.BindVertexBuffer(0, quad);
            const FlatPush push;
            cmd.SetPushConstants(&push, sizeof(push));
            cmd.Draw(6);
        });
        CheckPixel(At(pixels, 4, 2, 2), 128, 64, 0, 255, 2, "straight alpha over red");
    }

    {
        // Additive: out = src * a + dst. Half-green over half-red clamps nowhere.
        auto pipeline = FlatPipeline(BlendMode::Additive);
        auto quad = FullQuad(0.0f, 1.0f, 0.0f, 0.5f);

        auto pixels = RenderToPixels(4, 4, Color{ 0.5f, 0.0f, 0.0f, 1.0f },
                                     [&](CommandList& cmd) {
            cmd.BindPipeline(pipeline);
            cmd.BindVertexBuffer(0, quad);
            const FlatPush push;
            cmd.SetPushConstants(&push, sizeof(push));
            cmd.Draw(6);
        });
        CheckPixel(At(pixels, 4, 2, 2), 128, 128, 0, 255, 2, "additive over half red");
    }

    Section("indexed draw");

    {
        auto pipeline = FlatPipeline(BlendMode::None);
        const FlatVertex corners[4] = {
            { -1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 1.0f },
            {  1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 1.0f },
            {  1.0f,  1.0f, 1.0f, 1.0f, 0.0f, 1.0f },
            { -1.0f,  1.0f, 1.0f, 1.0f, 0.0f, 1.0f },
        };
        auto vertices = Gpu().CreateBuffer(BufferDesc{
            .size = sizeof(corners), .usage = BufferUsage::Vertex,
            .memory = MemoryKind::HostVisible, .debugName = "indexed-verts" });
        vertices->Upload(corners, sizeof(corners));

        // 16-bit indices, to exercise the IndexType the port added.
        const u16 indices[6] = { 0, 1, 2, 2, 3, 0 };
        auto indexBuffer = Gpu().CreateBuffer(BufferDesc{
            .size = sizeof(indices), .usage = BufferUsage::Index,
            .memory = MemoryKind::HostVisible, .debugName = "indexed-idx" });
        indexBuffer->Upload(indices, sizeof(indices));

        auto pixels = RenderToPixels(8, 8, Color{ 0.0f, 0.0f, 0.0f, 1.0f },
                                     [&](CommandList& cmd) {
            cmd.BindPipeline(pipeline);
            cmd.BindVertexBuffer(0, vertices);
            cmd.BindIndexBuffer(indexBuffer, IndexType::U16);
            const FlatPush push;
            cmd.SetPushConstants(&push, sizeof(push));
            cmd.DrawIndexed(6);
        });
        CheckPixel(At(pixels, 8, 4, 4), 255, 255, 0, 255, 1, "u16 indexed quad");
    }

    Section("shader capabilities");

    {
        // A fragment shader using `discard`. glslc emits the DemoteToHelperInvocation SPIR-V
        // capability for it, and creating the module fails outright unless the matching 1.3
        // device feature was enabled -- which is a whole class of bug a pixel comparison
        // cannot see, because there is no image to compare.
        PipelineDesc desc;
        desc.vertex   = FlatVert();
        desc.fragment = Gpu().CreateShader(ShaderDesc{
            .spirv = std::vector<u32>(std::begin(kDiscardFrag), std::end(kDiscardFrag)),
            .stage = ShaderStage::Fragment, .debugName = "discard.frag" });
        desc.vertexBuffers = { VertexBufferLayout{
            .stride = sizeof(FlatVertex),
            .attributes = {
                { 0, Format::RG32_SFLOAT,   offsetof(FlatVertex, x) },
                { 1, Format::RGBA32_SFLOAT, offsetof(FlatVertex, r) },
            } } };
        desc.pushConstantSize = sizeof(FlatPush);
        desc.blend       = BlendMode::None;
        desc.colorFormat = Format::RGBA8_UNORM;
        desc.debugName   = "discard";
        auto pipeline = Gpu().CreatePipeline(desc);
        CHECK(pipeline != nullptr);

        // Alpha below the shader's threshold: every fragment is discarded and the clear
        // survives. Above it, the quad is drawn.
        auto transparent = FullQuad(0.0f, 1.0f, 0.0f, 0.25f);
        auto opaque      = FullQuad(0.0f, 1.0f, 0.0f, 1.0f);

        auto draw = [&](const Ref<Buffer>& quad) {
            return RenderToPixels(4, 4, Color{ 1.0f, 0.0f, 0.0f, 1.0f },
                                  [&](CommandList& cmd) {
                cmd.BindPipeline(pipeline);
                cmd.BindVertexBuffer(0, quad);
                const FlatPush push;
                cmd.SetPushConstants(&push, sizeof(push));
                cmd.Draw(6);
            });
        };
        CheckPixel(At(draw(transparent), 4, 2, 2), 255, 0, 0, 255, 1, "discarded, clear survives");
        CheckPixel(At(draw(opaque), 4, 2, 2), 0, 255, 0, 255, 1, "kept, quad drawn");
    }

    Section("texture bindings");

    {
        // A four-slot sampler array with only two slots filled. hearth pads the rest, and the
        // point of the test is that sampling slot 1 returns slot 1 -- a padding bug that
        // overwrote real entries would show up here as the wrong colour.
        auto blue    = Gpu().CreateSolidTexture(0xFFFF0000u);   // 0xAABBGGRR
        auto magenta = Gpu().CreateSolidTexture(0xFFFF00FFu);

        PipelineDesc desc;
        desc.vertex = Gpu().CreateShader(ShaderDesc{
            .spirv = std::vector<u32>(std::begin(kTexturedVert), std::end(kTexturedVert)),
            .stage = ShaderStage::Vertex, .debugName = "textured.vert" });
        desc.fragment = Gpu().CreateShader(ShaderDesc{
            .spirv = std::vector<u32>(std::begin(kTexturedFrag), std::end(kTexturedFrag)),
            .stage = ShaderStage::Fragment, .debugName = "textured.frag" });
        desc.vertexBuffers = { VertexBufferLayout{
            .stride = sizeof(TexturedVertex),
            .attributes = {
                { 0, Format::RG32_SFLOAT, offsetof(TexturedVertex, x) },
                { 1, Format::RG32_SFLOAT, offsetof(TexturedVertex, u) },
            } } };
        desc.bindings = { BindingSlot{
            .binding = 0, .type = BindingType::SampledTexture, .count = 4,
            .stages = StageBit(ShaderStage::Fragment) } };
        desc.pushConstantSize = sizeof(u32);
        desc.blend       = BlendMode::None;
        desc.colorFormat = Format::RGBA8_UNORM;
        desc.debugName   = "textured";
        auto pipeline = Gpu().CreatePipeline(desc);

        auto group = Gpu().CreateBindGroup(pipeline, { BindGroupEntry{
            .binding = 0, .type = BindingType::SampledTexture,
            .textures = { blue, magenta } } });        // 2 of 4 -- the rest get padded

        const TexturedVertex quad[6] = {
            { -1.0f, -1.0f, 0.0f, 0.0f }, { 1.0f, -1.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f },
            {  1.0f,  1.0f, 1.0f, 1.0f }, { -1.0f, 1.0f, 0.0f, 1.0f }, { -1.0f, -1.0f, 0.0f, 0.0f },
        };
        auto vertices = Gpu().CreateBuffer(BufferDesc{
            .size = sizeof(quad), .usage = BufferUsage::Vertex,
            .memory = MemoryKind::HostVisible, .debugName = "tex-quad" });
        vertices->Upload(quad, sizeof(quad));

        auto sample = [&](u32 slot) {
            return RenderToPixels(4, 4, Color{ 0.0f, 0.0f, 0.0f, 1.0f },
                                  [&](CommandList& cmd) {
                cmd.BindPipeline(pipeline);
                cmd.BindBindGroup(group);
                cmd.BindVertexBuffer(0, vertices);
                cmd.SetPushConstants(&slot, sizeof(slot));
                cmd.Draw(6);
            });
        };

        CheckPixel(At(sample(0), 4, 2, 2), 0, 0, 255, 255, 2, "array slot 0 is blue");
        CheckPixel(At(sample(1), 4, 2, 2), 255, 0, 255, 255, 2, "array slot 1 is magenta");
        // Slot 2 was never written; hearth padded it with the first entry rather than
        // leaving a descriptor the shader must not read.
        CheckPixel(At(sample(2), 4, 2, 2), 0, 0, 255, 255, 2, "padded slot 2 falls back to slot 0");

        // Rewriting a bound group in place is the update-after-bind path a batcher needs.
        group->Update({ BindGroupEntry{
            .binding = 0, .type = BindingType::SampledTexture,
            .textures = { magenta, blue } } });
        CheckPixel(At(sample(0), 4, 2, 2), 255, 0, 255, 255, 2, "slot 0 after Update is magenta");
    }
}

}
