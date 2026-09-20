#pragma once

#include <cstdint>
#include <memory>

namespace hearth {

    using u8  = std::uint8_t;
    using u16 = std::uint16_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;
    using i8  = std::int8_t;
    using i16 = std::int16_t;
    using i32 = std::int32_t;
    using i64 = std::int64_t;
    using f32 = float;
    using f64 = double;

    template<typename T> using Scope = std::unique_ptr<T>;
    template<typename T> using Ref   = std::shared_ptr<T>;

    template<typename T, typename... Args>
    constexpr Scope<T> CreateScope(Args&&... args) {
        return std::make_unique<T>(std::forward<Args>(args)...);
    }

    template<typename T, typename... Args>
    constexpr Ref<T> CreateRef(Args&&... args) {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }

    struct Color {
        f32 r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
    };

}
