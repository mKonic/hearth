#include "Harness.h"
#include "Shaders.h"

#include <hearth/surface/HeadlessSurface.h>

#include <vector>

namespace hearth::tests {

namespace {

struct FlatVertex { f32 x, y, r, g, b, a; };
struct DepthPush  { f32 offsetX = 0, offsetY = 0, scaleX = 1, scaleY = 1, depth = 0; };

// A swapchain capture is BGRA or RGBA depending on the driver; read it as colour either way.
Pixel ReadCapture(const FrameCapture& frame, u32 x, u32 y) {
    const u8* p = frame.pixels.data() + (size_t(y) * frame.width + x) * 4;
    if (frame.format == Format::BGRA8_UNORM || frame.format == Format::BGRA8_SRGB)
        return Pixel{ p[2], p[1], p[0], p[3] };
    return Pixel{ p[0], p[1], p[2], p[3] };
}

} // namespace

void RunSwapchainTests() {
    Section("swapchain");

    if (!HeadlessSurface::Supported()) {
        std::printf("  (no VK_EXT_headless_surface on this machine; skipped -- lavapipe has it)\n");
        return;
    }

    HeadlessSurface surface(64, 48);
    DeviceDesc desc;
    desc.surface = &surface;
    desc.appName = "hearth-tests-swapchain";
    desc.enableValidation = true;
    desc.swapchainDepthFormat = Format::D32_SFLOAT;
    desc.swapchainSamples = 4;
    auto device = CreateDevice(desc);
    CHECK(device != nullptr);
    if (!device) return;

    Swapchain* swapchain = device->GetSwapchain();
    CHECK(swapchain != nullptr);
    if (!swapchain) return;
    CHECK(swapchain->Width() == 64 && swapchain->Height() == 48);
    CHECK(swapchain->DepthFormat() == Format::D32_SFLOAT);
    CHECK(swapchain->Samples() == device->Caps().SupportedSamples(4));

    auto shader = [&](const u32* begin, const u32* end, ShaderStage stage) {
        return device->CreateShader(ShaderDesc{ .spirv = std::vector<u32>(begin, end), .stage = stage });
    };
    PipelineDesc pd;
    pd.vertex = shader(std::begin(kDepthVert), std::end(kDepthVert), ShaderStage::Vertex);
    pd.fragment = shader(std::begin(kFlatFrag), std::end(kFlatFrag), ShaderStage::Fragment);
    pd.vertexBuffers = { VertexBufferLayout{
        .stride = sizeof(FlatVertex),
        .attributes = { { 0, Format::RG32_SFLOAT, 0 }, { 1, Format::RGBA32_SFLOAT, 8 } } } };
    pd.pushConstantSize = sizeof(DepthPush);
    pd.blend = BlendMode::None;
    pd.colorFormats = { device->SwapchainFormat() };
    pd.depthFormat = swapchain->DepthFormat();
    pd.samples = swapchain->Samples();
    pd.depthTest = true;
    pd.depthWrite = true;
    pd.depthCompare = CompareOp::Less;
    pd.debugName = "swapchain-depth";
    auto pipeline = device->CreatePipeline(pd);

    // A full quad and a lower-right triangle, each in one colour.
    auto mesh = [&](std::vector<FlatVertex> v) {
        auto buffer = device->CreateBuffer(BufferDesc{
            .size = v.size() * sizeof(FlatVertex), .usage = BufferUsage::Vertex,
            .memory = MemoryKind::HostVisible });
        buffer->Upload(v.data(), v.size() * sizeof(FlatVertex));
        return buffer;
    };
    auto quad = [&](f32 r, f32 g, f32 b) {
        return mesh({ { -1, -1, r, g, b, 1 }, { 1, -1, r, g, b, 1 }, { 1, 1, r, g, b, 1 },
                      { 1, 1, r, g, b, 1 }, { -1, 1, r, g, b, 1 }, { -1, -1, r, g, b, 1 } });
    };
    auto green = quad(0, 1, 0);
    auto blue = quad(0, 0, 1);
    auto triangle = mesh({ { 1, -1, 0, 1, 0, 1 }, { 1, 1, 0, 1, 0, 1 }, { -1, 1, 0, 1, 0, 1 } });

    auto draw = [&](CommandList& cmd, const Ref<Buffer>& vertices, u32 count, f32 depth,
                    f32 offsetX = 0, f32 scaleX = 1) {
        cmd.BindPipeline(pipeline);
        cmd.BindVertexBuffer(0, vertices);
        DepthPush push;
        push.depth = depth;
        push.offsetX = offsetX;
        push.scaleX = scaleX;
        cmd.SetPushConstants(&push, sizeof(push));
        cmd.Draw(count);
    };

    // Renders one frame and returns what was presented.
    auto frame = [&](const std::function<void(CommandList&)>& record) {
        FrameCapture captured;
        for (int attempt = 0; attempt < 4 && captured.pixels.empty(); ++attempt) {
            CommandList* cmd = device->BeginFrame();
            if (!cmd) continue;               // a rebuilt chain skips one frame
            device->CaptureNextFrame([&](const FrameCapture& f) { captured = f; });
            record(*cmd);
            device->EndFrame();
        }
        return captured;
    };

    {
        // Depth: green at 0.5 hides blue drawn after it at 0.75.
        FrameCapture f = frame([&](CommandList& cmd) {
            RenderPassDesc pass;
            pass.clearColor = Color{ 1, 0, 0, 1 };
            cmd.BeginRenderPass(pass);
            draw(cmd, green, 6, 0.5f);
            draw(cmd, blue, 6, 0.75f);
            cmd.EndRenderPass();
        });
        CHECK(f.width == 64 && f.height == 48 && f.pixels.size() == 64u * 48u * 4u);
        if (!f.pixels.empty()) {
            const Pixel p = ReadCapture(f, 32, 24);
            CHECK_MSG(p.g > 250 && p.b < 5 && p.r < 5, "depth-tested centre was ({},{},{})",
                      p.r, p.g, p.b);
        }
    }

    if (swapchain->Samples() > 1) {
        // MSAA: the triangle's diagonal edge resolves to in-between colours somewhere.
        FrameCapture f = frame([&](CommandList& cmd) {
            RenderPassDesc pass;
            pass.clearColor = Color{ 0, 0, 0, 1 };
            cmd.BeginRenderPass(pass);
            draw(cmd, triangle, 3, 0.5f);
            cmd.EndRenderPass();
        });
        int blended = 0;
        for (u32 y = 0; y < f.height && !f.pixels.empty(); ++y)
            for (u32 x = 0; x < f.width; ++x) {
                const u8 g = ReadCapture(f, x, y).g;
                if (g > 30 && g < 225) ++blended;
            }
        CHECK_MSG(blended > 0, "no antialiased pixels along the edge with {}x MSAA",
                  swapchain->Samples());
    }

    {
        // Two passes in one frame: the second loads what the first stored, multisampled
        // image included. Left half green in pass one, right half blue in pass two.
        FrameCapture f = frame([&](CommandList& cmd) {
            RenderPassDesc first;
            first.clearColor = Color{ 1, 0, 0, 1 };
            cmd.BeginRenderPass(first);
            draw(cmd, green, 6, 0.5f, -0.5f, 0.5f);
            cmd.EndRenderPass();
            RenderPassDesc second;
            second.loadOp = LoadOp::Load;
            cmd.BeginRenderPass(second);
            draw(cmd, blue, 6, 0.5f, 0.5f, 0.5f);
            cmd.EndRenderPass();
        });
        if (!f.pixels.empty()) {
            const Pixel left = ReadCapture(f, 16, 24), right = ReadCapture(f, 48, 24);
            CHECK_MSG(left.g > 250 && left.r < 5, "left half after the loading pass was ({},{},{})",
                      left.r, left.g, left.b);
            CHECK_MSG(right.b > 250 && right.r < 5, "right half was ({},{},{})",
                      right.r, right.g, right.b);
        }
    }

    {
        // A resize rebuilds the chain and its depth and multisampled images with it.
        surface.SetSize(80, 60);
        device->OnWindowResize(80, 60);
        CHECK(swapchain->Width() == 80 && swapchain->Height() == 60);
        FrameCapture f = frame([&](CommandList& cmd) {
            RenderPassDesc pass;
            pass.clearColor = Color{ 1, 0, 0, 1 };
            cmd.BeginRenderPass(pass);
            draw(cmd, green, 6, 0.5f);
            cmd.EndRenderPass();
        });
        CHECK(f.width == 80 && f.height == 60);
        if (!f.pixels.empty()) CHECK(ReadCapture(f, 79, 59).g > 250);
    }

    device->WaitIdle();
}

}
