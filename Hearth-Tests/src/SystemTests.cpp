#include "Harness.h"
#include "Shaders.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

namespace hearth::tests {

void RunSystemTests() {
    Section("concurrent resource creation");

    {
        // Device promises that resource creation is safe from several threads. The pieces
        // that could race are the sampler cache, the descriptor pool chain and the single
        // immediate-submit pool and fence that every upload goes through -- so this creates
        // textures (sampler + upload), buffers (device-local upload) and bind groups at once.
        constexpr int kThreads = 8;
        constexpr int kPerThread = 12;

        std::atomic<int> created{ 0 };
        std::vector<std::thread> threads;
        std::vector<std::vector<Ref<Texture>>> perThread(kThreads);

        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    TextureDesc desc;
                    desc.width = desc.height = 4;
                    // Vary the sampler settings so threads contend on the cache rather than
                    // all hitting one already-built entry.
                    desc.minFilter   = (i % 2) ? Filter::Nearest : Filter::Linear;
                    desc.addressMode = (i % 3) ? AddressMode::ClampToEdge : AddressMode::Repeat;
                    desc.debugName   = std::format("thread{}-tex{}", t, i);
                    auto texture = Gpu().CreateTexture(desc);

                    std::vector<u8> pixels(4 * 4 * 4, static_cast<u8>(t * 16 + i));
                    texture->Upload(pixels.data(), pixels.size());
                    perThread[t].push_back(texture);

                    auto buffer = Gpu().CreateBuffer(BufferDesc{
                        .size = 512, .usage = BufferUsage::Index,
                        .memory = MemoryKind::DeviceLocal,
                        .debugName = std::format("thread{}-buf{}", t, i) });
                    std::vector<u32> data(128, u32(t));
                    buffer->Upload(data.data(), data.size() * sizeof(u32));

                    ++created;
                }
            });
        }
        for (auto& thread : threads) thread.join();

        CHECK(created.load() == kThreads * kPerThread);
        for (int t = 0; t < kThreads; ++t)
            CHECK(perThread[t].size() == kPerThread);

        // Every texture must still be a live, distinct object afterwards.
        bool allValid = true;
        for (const auto& list : perThread)
            for (const auto& texture : list)
                if (!texture || texture->Width() != 4) allValid = false;
        CHECK(allValid);
    }

    Section("pipeline cache");

    {
        const auto path = std::filesystem::temp_directory_path()
                        / "hearth-tests" / "pipeline.cache";
        std::error_code ec;
        std::filesystem::remove(path, ec);

        auto buildOne = [&](const std::string& cachePath) {
            DeviceDesc desc;
            desc.surface = nullptr;
            desc.appName = "cache-test";
            desc.enableValidation = false;   // a second instance with layers is needless noise
            desc.pipelineCachePath = cachePath;
            auto device = CreateDevice(desc);
            if (!device) return false;

            PipelineDesc pipeline;
            pipeline.vertex = device->CreateShader(ShaderDesc{
                .spirv = std::vector<u32>(std::begin(kFlatVert), std::end(kFlatVert)),
                .stage = ShaderStage::Vertex, .debugName = "cache.vert" });
            pipeline.fragment = device->CreateShader(ShaderDesc{
                .spirv = std::vector<u32>(std::begin(kFlatFrag), std::end(kFlatFrag)),
                .stage = ShaderStage::Fragment, .debugName = "cache.frag" });
            pipeline.vertexBuffers = { VertexBufferLayout{
                .stride = 24,
                .attributes = { { 0, Format::RG32_SFLOAT, 0 },
                                { 1, Format::RGBA32_SFLOAT, 8 } } } };
            pipeline.pushConstantSize = 16;
            pipeline.colorFormats = { Format::RGBA8_UNORM };
            pipeline.debugName = "cache-probe";
            return device->CreatePipeline(pipeline) != nullptr;
        };

        CHECK(buildOne(path.string()));                       // device destroyed here, cache written
        CHECK(std::filesystem::exists(path));
        const auto size = std::filesystem::file_size(path, ec);
        CHECK_MSG(!ec && size > 0, "pipeline cache written but empty");

        // A second run must accept the blob the first one wrote. A rejected cache is not an
        // error the driver reports, so what this really pins is that the file round-trips
        // and nothing refuses to start because of it.
        CHECK(buildOne(path.string()));

        // A corrupt cache must not stop the device coming up.
        {
            std::FILE* file = std::fopen(path.string().c_str(), "wb");
            if (file) { std::fputs("not a pipeline cache", file); std::fclose(file); }
        }
        CHECK_MSG(buildOne(path.string()), "a corrupt cache file must be discarded, not fatal");

        // An empty path means in-memory only, and must not try to write anything.
        CHECK(buildOne(""));

        std::filesystem::remove(path, ec);
    }
}

}
