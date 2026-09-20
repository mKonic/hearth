#pragma once

#include "hearth/Base.h"

#include <vulkan/vulkan.h>

#include <vector>

namespace hearth {

    // Everything hearth asks of a window, and the only place a windowing library appears in this
    // whole library -- which is the point. hearth links neither GLFW nor SDL nor an Android
    // runtime; the host implements these three calls against whatever it already uses, and the
    // Vulkan backend behind them is unchanged on every system, because the driver is.
    //
    // Ready-made implementations for GLFW, SDL3 and Android live in hearth/surface/ as
    // header-only adapters. They are not compiled into the library; include the one you want in
    // one of your own translation units, or write the three calls yourself.
    class Surface {
    public:
        virtual ~Surface() = default;

        // Creates the presentation surface. Called once at device creation, and AGAIN after
        // OnSurfaceLost -- an Android activity that returns to the foreground gets a different
        // ANativeWindow than the one it left with, so this must work more than once.
        virtual VkResult CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) = 0;

        // The drawable size in PIXELS. Not the window's logical size: on a HiDPI display those
        // differ by the scale factor, and a swapchain built at the logical size renders a
        // quarter-resolution image stretched over the window.
        virtual void FramebufferSize(u32& width, u32& height) const = 0;

        // Instance extensions this surface needs beyond VK_KHR_surface. Most hosts return nothing:
        // vk-bootstrap already enables the platform extension for the system it was compiled for.
        // SDL and GLFW can both report a set that differs from that guess (an SDL build running on
        // XWayland, say), and this is where it gets through.
        virtual std::vector<const char*> RequiredInstanceExtensions() const { return {}; }
    };

}
