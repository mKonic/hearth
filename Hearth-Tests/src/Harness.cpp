#include "Harness.h"

#include <cstdlib>
#include <cstring>

namespace hearth::tests {

    namespace {
        Results     s_Results;
        const char* s_Section = "";
    }

    Results& Tally() { return s_Results; }

    void Section(const char* name) {
        s_Section = name;
        std::printf("%s\n", name);
    }

    void Fail(const char* file, int line, const std::string& message) {
        ++s_Results.failed;
        std::printf("  FAIL [%s] %s:%d: %s\n", s_Section, file, line, message.c_str());
    }

    Device& Gpu() {
        // Leaked on purpose: the device outlives every test and tearing it down at static
        // destruction time would race the Vulkan loader's own atexit handlers.
        static Device* device = [] {
            DeviceDesc desc;
            desc.surface = nullptr;          // headless: no window, no swapchain
            desc.appName = "hearth-tests";
            desc.enableValidation = true;    // the suite exists to catch exactly this
            auto owned = CreateDevice(desc);
            if (!owned) {
                std::printf("FATAL: no usable Vulkan device. On a machine without a GPU, try\n"
                            "  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json\n");
                std::exit(1);
            }
            return owned.release();
        }();
        return *device;
    }

    std::vector<Pixel> RenderToPixels(u32 width, u32 height, Color clear,
                                      const std::function<void(CommandList&)>& record) {
        RenderTargetDesc desc;
        desc.width  = width;
        desc.height = height;
        // RGBA, so a readback needs no channel swizzling and an assertion reads in the same
        // order the colour was written.
        desc.colorFormat = Format::RGBA8_UNORM;
        desc.debugName   = "test-target";
        auto target = Gpu().CreateRenderTarget(desc);

        Gpu().SubmitImmediate([&](CommandList& cmd) {
            RenderPassDesc pass;
            pass.target     = target.get();
            pass.loadOp     = LoadOp::Clear;
            pass.clearColor = clear;
            cmd.BeginRenderPass(pass);
            if (record) record(cmd);
            cmd.EndRenderPass();
        });

        std::vector<Pixel> pixels(static_cast<size_t>(width) * height);
        target->ReadPixels(pixels.data(), pixels.size() * sizeof(Pixel));
        return pixels;
    }

}

int main() {
    using namespace hearth::tests;

    // Line-buffered even when redirected to a file. A driver crash mid-suite takes the
    // process down without flushing, and block buffering would then throw away every line
    // printed before it -- leaving a CI log that says nothing about where it died.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // A validation error is a test failure, not a log line nobody reads.
    hearth::SetLogSink([](hearth::LogLevel level, std::string_view message, void*) {
        if (level == hearth::LogLevel::Error) {
            ++Tally().failed;
            std::printf("  FAIL [hearth] %.*s\n", int(message.size()), message.data());
        } else if (std::getenv("HEARTH_TEST_VERBOSE")) {
            std::printf("  [hearth] %.*s\n", int(message.size()), message.data());
        }
    });

    RunDeviceTests();
    RunResourceTests();
    RunRenderTests();
    RunComputeTests();
    RunSystemTests();

    // A green run means much less without the validation layers: most of what this suite is
    // positioned to catch -- bad barriers, unwritten descriptors, layout mismatches -- is
    // reported by them and not by a pixel comparison. Say which kind of run this was rather
    // than letting "0 failed" imply the stronger one.
    std::printf("\n%d checks, %d failed%s\n", Tally().checks, Tally().failed,
                Gpu().Caps().validationActive
                    ? ""
                    : "  (validation layers absent -- install them for a full run)");
    return Tally().failed == 0 ? 0 : 1;
}
