#pragma once

#include "hearth/Base.h"

#include <format>
#include <string>
#include <string_view>

namespace hearth {

    enum class LogLevel : u8 { Trace, Info, Warn, Error };

    // hearth does not pick a logging library. It formats a line and hands it to whatever the host
    // installed; the default writes to stderr with a [hearth] prefix. A host with spdlog routes it
    // into spdlog in three lines and its GPU messages land in the same file as everything else.
    using LogSink = void (*)(LogLevel level, std::string_view message, void* user);

    void SetLogSink(LogSink sink, void* user = nullptr);
    void LogMessage(LogLevel level, std::string_view message);

    template<typename... Args>
    void LogFormatted(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
        LogMessage(level, std::format(fmt, std::forward<Args>(args)...));
    }

    // Reported once, from the one place a fatal misuse is detected, then the caller decides.
    // hearth never calls std::abort on its own: a host with a crash reporter wants the callback.
    using AbortHandler = void (*)(std::string_view message, void* user);
    void SetAbortHandler(AbortHandler handler, void* user = nullptr);
    [[noreturn]] void Abort(std::string_view message);

}

#define HEARTH_TRACE(...) ::hearth::LogFormatted(::hearth::LogLevel::Trace, __VA_ARGS__)
#define HEARTH_INFO(...)  ::hearth::LogFormatted(::hearth::LogLevel::Info,  __VA_ARGS__)
#define HEARTH_WARN(...)  ::hearth::LogFormatted(::hearth::LogLevel::Warn,  __VA_ARGS__)
#define HEARTH_ERROR(...) ::hearth::LogFormatted(::hearth::LogLevel::Error, __VA_ARGS__)

// Stays in release builds. Everything it guards is a programming error in the *caller* -- drawing
// without a pipeline bound, nesting render passes -- and the Vulkan validation layers that would
// otherwise catch it are off in a shipped build, where the same mistake is a silent GPU hang.
#define HEARTH_ASSERT(cond, ...)                                                                  \
    do {                                                                                          \
        if (!(cond)) [[unlikely]]                                                                 \
            ::hearth::Abort(std::format("{}:{}: {}", __FILE__, __LINE__,                          \
                                        std::format(__VA_ARGS__)));                               \
    } while (0)
