#include "Harness.h"
#include "Shaders.h"

#include <vector>

namespace hearth::tests {

namespace {

struct FlatVertex { f32 x, y, r, g, b, a; };
struct DepthPush  { f32 offsetX = 0, offsetY = 0, scaleX = 1, scaleY = 1, depth = 0; };

Ref<Shader> Make(const u32* begin, const u32* end, ShaderStage stage, const char* name) {
    return Gpu().CreateShader(ShaderDesc{ .spirv = std::vector<u32>(begin, end),
                                          .stage = stage, .debugName = name });
}

Ref<Buffer> Quad(f32 r, f32 g, f32 b, f32 a) {
    const FlatVertex v[6] = {
        { -1, -1, r, g, b, a }, {  1, -1, r, g, b, a }, {  1,  1, r, g, b, a },
        {  1,  1, r, g, b, a }, { -1,  1, r, g, b, a }, { -1, -1, r, g, b, a },
    };
    auto buffer = Gpu().CreateBuffer(BufferDesc{
        .size = sizeof(v), .usage = BufferUsage::Vertex,
        .memory = MemoryKind::HostVisible, .debugName = "state-quad" });
    buffer->Upload(v, sizeof(v));
    return buffer;
}

PipelineDesc Base(Format depth = Format::Undefined, u32 samples = 1) {
    // Leaked, like the device: a static destructor would call Vulkan at exit, after the
    // validation layer has torn down its own state.
    static Ref<Shader>* vert = new Ref<Shader>(Make(std::begin(kDepthVert), std::end(kDepthVert),
                                                    ShaderStage::Vertex, "depth.vert"));
    static Ref<Shader>* frag = new Ref<Shader>(Make(std::begin(kFlatFrag), std::end(kFlatFrag),
                                                    ShaderStage::Fragment, "flat.frag"));
    PipelineDesc desc;
    desc.vertex = *vert;
    desc.fragment = *frag;
    desc.vertexBuffers = { VertexBufferLayout{
        .stride = sizeof(FlatVertex),
        .attributes = { { 0, Format::RG32_SFLOAT, 0 }, { 1, Format::RGBA32_SFLOAT, 8 } } } };
    desc.pushConstantSize = sizeof(DepthPush);
    desc.blend = BlendMode::None;
    desc.colorFormats = { Format::RGBA8_UNORM };
    desc.depthFormat = depth;
    desc.samples = samples;
    desc.debugName = "state";
    return desc;
}

struct Draw {
    Ref<Pipeline> pipeline;
    Ref<Buffer> quad;
    f32 depth = 0.0f;
};

// Draws each entry in order into one pass over a fresh 4x4 target and returns the centre
// pixel. Red clear, so "nothing drew" reads as red.
Pixel Render(const std::vector<Draw>& draws, Format depth = Format::Undefined,
             u32 samples = 1, f32 clearDepth = 1.0f) {
    RenderTargetDesc desc;
    desc.width = desc.height = 4;
    desc.colorFormats = { Format::RGBA8_UNORM };
    desc.depthFormat = depth;
    desc.samples = samples;
    desc.debugName = "state-target";
    auto target = Gpu().CreateRenderTarget(desc);

    Gpu().SubmitImmediate([&](CommandList& cmd) {
        RenderPassDesc pass;
        pass.target = target.get();
        pass.clearColor = Color{ 1, 0, 0, 1 };
        pass.clearDepth = clearDepth;
        cmd.BeginRenderPass(pass);
        for (const Draw& d : draws) {
            cmd.BindPipeline(d.pipeline);
            cmd.BindVertexBuffer(0, d.quad);
            DepthPush push;
            push.depth = d.depth;
            cmd.SetPushConstants(&push, sizeof(push));
            cmd.Draw(6);
        }
        cmd.EndRenderPass();
    });

    std::vector<Pixel> pixels(16);
    target->ReadPixels(pixels.data(), pixels.size() * sizeof(Pixel));
    return At(pixels, 4, 2, 2);
}

bool Green(const Pixel& p) { return p.g > 200 && p.r < 50; }

} // namespace

void RunPipelineStateTests() {
    auto green = Quad(0, 1, 0, 1);

    Section("cull");

    {
        // The quad's winding is whatever it is; what must hold is that culling back faces
        // keeps it under exactly one of the two front-face conventions, and that culling
        // front faces flips that.
        auto drawn = [&](CullMode cull, FrontFace front) {
            PipelineDesc desc = Base();
            desc.cull = cull;
            desc.frontFace = front;
            return Green(Render({ { Gpu().CreatePipeline(desc), green } }));
        };
        CHECK(drawn(CullMode::None, FrontFace::CounterClockwise));
        CHECK(drawn(CullMode::None, FrontFace::Clockwise));
        const bool backCcw = drawn(CullMode::Back, FrontFace::CounterClockwise);
        const bool backCw  = drawn(CullMode::Back, FrontFace::Clockwise);
        CHECK_MSG(backCcw != backCw, "back-face culling kept the quad under {} conventions",
                  backCcw ? "both" : "neither");
        CHECK(drawn(CullMode::Front, FrontFace::CounterClockwise) == !backCcw);
    }

    Section("depth compare");

    {
        auto drawn = [&](CompareOp op, f32 z, f32 clearDepth) {
            PipelineDesc desc = Base(Format::D32_SFLOAT);
            desc.depthTest = true;
            desc.depthCompare = op;
            return Green(Render({ { Gpu().CreatePipeline(desc), green, z } },
                                Format::D32_SFLOAT, 1, clearDepth));
        };
        CHECK(drawn(CompareOp::Less, 0.25f, 1.0f));
        CHECK(!drawn(CompareOp::Greater, 0.25f, 1.0f));
        CHECK(drawn(CompareOp::Greater, 0.75f, 0.5f));
        CHECK(!drawn(CompareOp::Never, 0.25f, 1.0f));
        CHECK(drawn(CompareOp::Always, 0.75f, 0.0f));
    }

    Section("depth bias");

    {
        // A blue quad lays down depth 0.5; a green one at the same depth then passes
        // LessOrEqual only if its bias does not push it behind.
        PipelineDesc first = Base(Format::D32_SFLOAT);
        first.depthTest = true;
        first.depthWrite = true;
        first.depthCompare = CompareOp::Always;
        auto writer = Gpu().CreatePipeline(first);
        auto blue = Quad(0, 0, 1, 1);

        auto secondPasses = [&](f32 constant) {
            PipelineDesc desc = Base(Format::D32_SFLOAT);
            desc.depthTest = true;
            desc.depthCompare = CompareOp::LessOrEqual;
            desc.depthBias.constant = constant;
            const Pixel p = Render({ { writer, blue, 0.5f },
                                     { Gpu().CreatePipeline(desc), green, 0.5f } },
                                   Format::D32_SFLOAT);
            return Green(p);
        };
        CHECK(secondPasses(0.0f));
        CHECK_MSG(!secondPasses(1000.0f), "a positive bias did not push the quad behind");
        CHECK(secondPasses(-1000.0f));
    }

    Section("alpha to coverage");

    {
        const u32 samples = Gpu().Caps().SupportedSamples(4);
        if (samples < 2) {
            std::printf("  (no multisampling on this device; skipped)\n");
        } else {
            // Alpha 0.5 with no blending: without coverage the pixel is fully green, with it
            // only some samples are written and the resolve averages them toward the red clear.
            auto half = Quad(0, 1, 0, 0.5f);
            PipelineDesc plain = Base(Format::Undefined, samples);
            PipelineDesc a2c = plain;
            a2c.alphaToCoverage = true;
            const Pixel off = Render({ { Gpu().CreatePipeline(plain), half } },
                                     Format::Undefined, samples);
            const Pixel on  = Render({ { Gpu().CreatePipeline(a2c), half } },
                                     Format::Undefined, samples);
            CHECK_MSG(off.g > 250, "without coverage green was {}", off.g);
            CHECK_MSG(on.g > 20 && on.g < 235 && on.r > 20,
                      "with coverage got ({},{},{}), expected a red/green mix", on.r, on.g, on.b);
        }
    }

    Section("frame index");

    {
        // A headless device still runs the frame loop, so the slot has to walk every index in
        // order and wrap: per-frame buffers are indexed by it.
        const u32 slots = Gpu().Caps().framesInFlight;
        std::vector<u32> seen;
        for (u32 i = 0; i < slots * 2; ++i) {
            CommandList* cmd = Gpu().BeginFrame();
            CHECK(cmd != nullptr);
            if (!cmd) break;
            seen.push_back(Gpu().FrameIndex());
            Gpu().EndFrame();
        }
        for (u32 i = 0; i < seen.size(); ++i)
            CHECK_MSG(seen[i] == i % slots, "frame {} used slot {}", i, seen[i]);
    }
}

}
