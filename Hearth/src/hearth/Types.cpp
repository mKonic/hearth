#include "hearth/Types.h"

namespace hearth {

    u32 FormatSize(Format f) {
        switch (f) {
            case Format::R8_UNORM:       return 1;
            case Format::RG8_UNORM:      return 2;
            case Format::RGBA8_UNORM:
            case Format::RGBA8_SRGB:
            case Format::BGRA8_UNORM:
            case Format::BGRA8_SRGB:     return 4;
            case Format::R16_SFLOAT:     return 2;
            case Format::RG16_SFLOAT:    return 4;
            case Format::RGBA16_SFLOAT:  return 8;
            case Format::R32_SFLOAT:
            case Format::R32_UINT:
            case Format::D32_SFLOAT:     return 4;
            case Format::RG32_SFLOAT:
            case Format::RG32_UINT:      return 8;
            case Format::RGB32_SFLOAT:   return 12;
            case Format::RGBA32_SFLOAT:
            case Format::RGBA32_UINT:    return 16;
            case Format::Undefined:      return 0;
        }
        return 0;
    }

    bool IsDepthFormat(Format f) { return f == Format::D32_SFLOAT; }

    const char* FormatName(Format f) {
        switch (f) {
            case Format::Undefined:     return "Undefined";
            case Format::R8_UNORM:      return "R8_UNORM";
            case Format::RG8_UNORM:     return "RG8_UNORM";
            case Format::RGBA8_UNORM:   return "RGBA8_UNORM";
            case Format::RGBA8_SRGB:    return "RGBA8_SRGB";
            case Format::BGRA8_UNORM:   return "BGRA8_UNORM";
            case Format::BGRA8_SRGB:    return "BGRA8_SRGB";
            case Format::R16_SFLOAT:    return "R16_SFLOAT";
            case Format::RG16_SFLOAT:   return "RG16_SFLOAT";
            case Format::RGBA16_SFLOAT: return "RGBA16_SFLOAT";
            case Format::R32_SFLOAT:    return "R32_SFLOAT";
            case Format::RG32_SFLOAT:   return "RG32_SFLOAT";
            case Format::RGB32_SFLOAT:  return "RGB32_SFLOAT";
            case Format::RGBA32_SFLOAT: return "RGBA32_SFLOAT";
            case Format::R32_UINT:      return "R32_UINT";
            case Format::RG32_UINT:     return "RG32_UINT";
            case Format::RGBA32_UINT:   return "RGBA32_UINT";
            case Format::D32_SFLOAT:    return "D32_SFLOAT";
        }
        return "<unknown>";
    }

}
