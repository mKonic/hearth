#pragma once

#include <cstdio>
#include <format>
#include <string>

// A test is a function that makes checks. No framework: the suite has to build and run on a
// bare CI box with nothing installed but a compiler and a Vulkan loader, and a dependency
// that has to be fetched is one more thing between a failure and someone seeing it.

namespace hearth::tests {

    struct Results {
        int checks = 0;
        int failed = 0;
    };

    Results& Tally();

    void Section(const char* name);
    void Fail(const char* file, int line, const std::string& message);

    inline void Record(bool ok, const char* file, int line, const std::string& message) {
        ++Tally().checks;
        if (!ok) Fail(file, line, message);
    }

}

#define CHECK(cond)                                                                           \
    ::hearth::tests::Record((cond), __FILE__, __LINE__, #cond)

#define CHECK_MSG(cond, ...)                                                                  \
    ::hearth::tests::Record((cond), __FILE__, __LINE__, std::format(__VA_ARGS__))

// Colour comparison with a tolerance, because a software rasterizer and a discrete GPU do
// not have to agree on the last bit of a blend, and a test that demands they do is a test
// that fails on somebody else's machine for no reason.
#define CHECK_NEAR(actual, expected, tolerance)                                               \
    ::hearth::tests::Record(                                                                  \
        (actual) >= (expected) - (tolerance) && (actual) <= (expected) + (tolerance),         \
        __FILE__, __LINE__,                                                                   \
        std::format("{} = {}, expected {} +/- {}", #actual, (actual), (expected), (tolerance)))
