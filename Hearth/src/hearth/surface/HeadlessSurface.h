#pragma once

// Header-only adapter: a real swapchain with no window, through VK_EXT_headless_surface.
// Presentation goes nowhere, which is what a CI run wants -- the whole swapchain path (acquire,
// present, resize, capture) exercised on a box with no display. Mesa's drivers, lavapipe
// included, provide it; check Supported() first.

#include "hearth/Surface.h"

#include <cstring>
#include <vector>

namespace hearth {

    class HeadlessSurface final : public Surface {
    public:
        HeadlessSurface(u32 width, u32 height) : m_Width(width), m_Height(height) {}

        // Whether the loader and a driver offer VK_EXT_headless_surface on this machine.
        static bool Supported() {
            u32 count = 0;
            if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS)
                return false;
            std::vector<VkExtensionProperties> extensions(count);
            vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
            for (const auto& e : extensions)
                if (std::strcmp(e.extensionName, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME) == 0)
                    return true;
            return false;
        }

        // The size the next swapchain build will use; pair it with Device::OnWindowResize.
        void SetSize(u32 width, u32 height) { m_Width = width; m_Height = height; }

        VkResult CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) override {
            const auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT"));
            if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
            VkHeadlessSurfaceCreateInfoEXT info{ VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
            return create(instance, &info, nullptr, outSurface);
        }

        void FramebufferSize(u32& width, u32& height) const override {
            width = m_Width;
            height = m_Height;
        }

        std::vector<const char*> RequiredInstanceExtensions() const override {
            return { VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME };
        }

    private:
        u32 m_Width = 0, m_Height = 0;
    };

}
