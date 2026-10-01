#pragma once

// Header-only adapter: include this in one of YOUR Objective-C++ translation units, for iOS or for
// a macOS host that does not go through GLFW or SDL. Vulkan reaches Metal through MoltenVK and
// VK_EXT_metal_surface; the layer is the view's CAMetalLayer.

#include "hearth/Surface.h"

#include <vulkan/vulkan_metal.h>

#import <QuartzCore/CAMetalLayer.h>

namespace hearth {

    class MetalSurface final : public Surface {
    public:
        explicit MetalSurface(CAMetalLayer* layer) : m_Layer(layer) {}

        void Reset(CAMetalLayer* layer) { m_Layer = layer; }

        VkResult CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) override {
            if (!m_Layer) return VK_ERROR_INITIALIZATION_FAILED;
            VkMetalSurfaceCreateInfoEXT info{ VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT };
            info.pLayer = m_Layer;
            return vkCreateMetalSurfaceEXT(instance, &info, nullptr, outSurface);
        }

        // drawableSize is in pixels already; the layer's bounds are in points.
        void FramebufferSize(u32& width, u32& height) const override {
            width = height = 0;
            if (!m_Layer) return;
            const CGSize size = m_Layer.drawableSize;
            width  = size.width  > 0 ? static_cast<u32>(size.width)  : 0u;
            height = size.height > 0 ? static_cast<u32>(size.height) : 0u;
        }

        // VK_KHR_portability_enumeration, which MoltenVK needs, is enabled by vk-bootstrap.
        std::vector<const char*> RequiredInstanceExtensions() const override {
            return { VK_EXT_METAL_SURFACE_EXTENSION_NAME };
        }

    private:
        CAMetalLayer* m_Layer = nullptr;
    };

}
