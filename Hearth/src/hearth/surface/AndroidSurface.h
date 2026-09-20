#pragma once

// Header-only adapter: include this in one of YOUR translation units on Android.
//
// The ANativeWindow here is NOT stable for the life of the app. It is destroyed when the activity
// leaves the foreground and a different one arrives on the way back, which is what
// Device::OnSurfaceLost and OnSurfaceRecreated exist for:
//
//     onNativeWindowDestroyed  ->  device->OnSurfaceLost();  surface.Reset(nullptr);
//     onNativeWindowCreated    ->  surface.Reset(window);    device->OnSurfaceRecreated(surface);

#include "hearth/Surface.h"

#include <android/native_window.h>
#include <vulkan/vulkan_android.h>

namespace hearth {

    class AndroidSurface final : public Surface {
    public:
        explicit AndroidSurface(ANativeWindow* window = nullptr) : m_Window(window) {}

        void Reset(ANativeWindow* window) { m_Window = window; }

        VkResult CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) override {
            if (!m_Window) return VK_ERROR_INITIALIZATION_FAILED;
            VkAndroidSurfaceCreateInfoKHR info{ VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
            info.window = m_Window;
            return vkCreateAndroidSurfaceKHR(instance, &info, nullptr, outSurface);
        }

        void FramebufferSize(u32& width, u32& height) const override {
            if (!m_Window) { width = height = 0; return; }
            const int w = ANativeWindow_getWidth(m_Window);
            const int h = ANativeWindow_getHeight(m_Window);
            width  = w > 0 ? static_cast<u32>(w) : 0u;
            height = h > 0 ? static_cast<u32>(h) : 0u;
        }

    private:
        ANativeWindow* m_Window = nullptr;
    };

}
