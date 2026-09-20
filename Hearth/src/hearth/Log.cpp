#include "hearth/Log.h"

#include <cstdio>
#include <cstdlib>

namespace hearth {

    namespace {

        void DefaultSink(LogLevel level, std::string_view message, void*) {
            const char* tag = "";
            switch (level) {
                case LogLevel::Trace: tag = "trace"; break;
                case LogLevel::Info:  tag = "info";  break;
                case LogLevel::Warn:  tag = "warn";  break;
                case LogLevel::Error: tag = "error"; break;
            }
            std::fprintf(level >= LogLevel::Warn ? stderr : stdout,
                         "[hearth %s] %.*s\n", tag,
                         static_cast<int>(message.size()), message.data());
        }

        [[noreturn]] void DefaultAbort(std::string_view message, void*) {
            std::fprintf(stderr, "[hearth fatal] %.*s\n",
                         static_cast<int>(message.size()), message.data());
            std::fflush(stderr);
            std::abort();
        }

        LogSink      s_Sink  = &DefaultSink;
        void*        s_SinkUser = nullptr;
        AbortHandler s_Abort = &DefaultAbort;
        void*        s_AbortUser = nullptr;

    }

    void SetLogSink(LogSink sink, void* user) {
        s_Sink = sink ? sink : &DefaultSink;
        s_SinkUser = user;
    }

    void LogMessage(LogLevel level, std::string_view message) {
        s_Sink(level, message, s_SinkUser);
    }

    void SetAbortHandler(AbortHandler handler, void* user) {
        s_Abort = handler ? handler : &DefaultAbort;
        s_AbortUser = user;
    }

    // A host handler that returns anyway would drop us into the very state the assert was
    // guarding, with the diagnosis already printed and nothing to attribute the crash to. Take
    // the process down here instead, where the message is still on screen.
    void Abort(std::string_view message) {
        s_Abort(message, s_AbortUser);
        DefaultAbort(message, nullptr);
    }

}
