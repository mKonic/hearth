#pragma once

// Header-only adapter: include this in one of YOUR translation units, where SDL3 is already on
// the include path. hearth itself does not compile or link against SDL.

#include "hearth/Log.h"
#include "hearth/Surface.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace hearth {

    class Sdl3Surface final : public Surface {
    public:
        explicit Sdl3Surface(SDL_Window* window) : m_Window(window) {}

        // For a window that was destroyed and remade. Pass the new one before
        // Device::OnSurfaceRecreated.
        void Reset(SDL_Window* window) { m_Window = window; }

        VkResult CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) override {
            if (!m_Window) return VK_ERROR_INITIALIZATION_FAILED;
            if (!SDL_Vulkan_CreateSurface(m_Window, instance, nullptr, outSurface)) {
                HEARTH_ERROR("SDL_Vulkan_CreateSurface: {}", SDL_GetError());
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            return VK_SUCCESS;
        }

        void FramebufferSize(u32& width, u32& height) const override {
            int w = 0, h = 0;
            if (m_Window) SDL_GetWindowSizeInPixels(m_Window, &w, &h);
            width  = w > 0 ? static_cast<u32>(w) : 0u;
            height = h > 0 ? static_cast<u32>(h) : 0u;
        }

        std::vector<const char*> RequiredInstanceExtensions() const override {
            Uint32 count = 0;
            const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
            if (!names) return {};
            return std::vector<const char*>(names, names + count);
        }

    private:
        SDL_Window* m_Window = nullptr;
    };

}
