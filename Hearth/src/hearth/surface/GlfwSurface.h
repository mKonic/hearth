#pragma once

// Header-only adapter: include this in one of YOUR translation units, where GLFW is already on
// the include path. hearth itself does not compile or link against GLFW.
//
// vulkan.h must come first, which hearth/Surface.h takes care of -- glfw3.h only declares the
// Vulkan entry points if it can already see the Vulkan types.

#include "hearth/Surface.h"

#include <GLFW/glfw3.h>

namespace hearth {

    class GlfwSurface final : public Surface {
    public:
        explicit GlfwSurface(GLFWwindow* window) : m_Window(window) {}

        void Reset(GLFWwindow* window) { m_Window = window; }

        VkResult CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) override {
            if (!m_Window) return VK_ERROR_INITIALIZATION_FAILED;
            return glfwCreateWindowSurface(instance, m_Window, nullptr, outSurface);
        }

        void FramebufferSize(u32& width, u32& height) const override {
            int w = 0, h = 0;
            if (m_Window) glfwGetFramebufferSize(m_Window, &w, &h);
            width  = w > 0 ? static_cast<u32>(w) : 0u;
            height = h > 0 ? static_cast<u32>(h) : 0u;
        }

        std::vector<const char*> RequiredInstanceExtensions() const override {
            uint32_t count = 0;
            const char** names = glfwGetRequiredInstanceExtensions(&count);
            if (!names) return {};
            return std::vector<const char*>(names, names + count);
        }

    private:
        GLFWwindow* m_Window = nullptr;
    };

}
