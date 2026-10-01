#pragma once

#include "hearth/Device.h"
#include "platform/vulkan/VulkanCommon.h"

#include <vector>

namespace hearth {

    class VulkanDevice;

    class VulkanSwapchain final : public Swapchain {
    public:
        VulkanSwapchain(VulkanDevice& device, VkSurfaceKHR surface, u32 width, u32 height, bool vsync,
                        Format depthFormat, u32 samples);
        ~VulkanSwapchain() override;

        u32 Width()  const override { return m_Width; }
        u32 Height() const override { return m_Height; }
        Format ColorFormat() const override { return FromVk(m_Format); }
        Format DepthFormat() const override { return m_DepthFormat; }
        u32 Samples() const override { return m_Samples; }

        // The multisampled colour image a pass draws into before resolving into the swapchain
        // image; VK_NULL_HANDLE when Samples() is 1. Depth likewise when there is none.
        VkImage     MsaaImage() const { return m_Msaa.image; }
        VkImageView MsaaView()  const { return m_Msaa.view; }
        VkImage     DepthImage() const { return m_Depth.image; }
        VkImageView DepthView()  const { return m_Depth.view; }

        void Resize(u32 width, u32 height);

        VkSwapchainKHR Raw() const { return m_Swapchain; }
        VkFormat    VkColorFormat() const { return m_Format; }
        VkImage     Image(u32 i) const { return m_Images[i]; }
        VkImageView View(u32 i)  const { return m_Views[i]; }
        VkSemaphore RenderFinished(u32 i) const { return m_RenderFinished[i]; }
        // The render-pass path's framebuffer for image i, made on first use.
        VkFramebuffer Framebuffer(u32 i);
        u32  ImageCount() const { return static_cast<u32>(m_Images.size()); }
        bool Valid() const { return m_Swapchain != VK_NULL_HANDLE && m_Width > 0 && m_Height > 0; }

    private:
        struct OwnedImage {
            VkImage image = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VmaAllocation allocation = nullptr;
        };

        void Build(u32 width, u32 height);
        void BuildAttachments();
        OwnedImage MakeImage(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect);
        void DestroyImage(OwnedImage& image);
        void Destroy();

        VulkanDevice&  m_Device;
        VkSurfaceKHR   m_Surface   = VK_NULL_HANDLE;
        VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
        VkFormat       m_Format    = VK_FORMAT_UNDEFINED;
        u32  m_Width = 0, m_Height = 0;
        bool m_VSync = true;
        Format m_DepthFormat = Format::Undefined;
        u32  m_Samples = 1;
        OwnedImage m_Msaa;
        OwnedImage m_Depth;

        std::vector<VkImage>     m_Images;
        std::vector<VkImageView> m_Views;
        // One per swapchain IMAGE, not per frame in flight: the semaphore a present waits on
        // belongs to the image being presented, and with two frames over three images a per-frame
        // semaphore gets signalled twice while an older present is still pending.
        std::vector<VkSemaphore> m_RenderFinished;
        std::vector<VkFramebuffer> m_Framebuffers;
    };

}
