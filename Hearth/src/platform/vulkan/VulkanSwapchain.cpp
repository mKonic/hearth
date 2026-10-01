#include "platform/vulkan/VulkanSwapchain.h"

#include "platform/vulkan/VulkanDevice.h"

#include <VkBootstrap.h>

namespace hearth {

    VulkanSwapchain::VulkanSwapchain(VulkanDevice& device, VkSurfaceKHR surface,
                                     u32 width, u32 height, bool vsync,
                                     Format depthFormat, u32 samples)
        : m_Device(device), m_Surface(surface), m_VSync(vsync), m_DepthFormat(depthFormat),
          m_Samples(device.Caps().SupportedSamples(samples ? samples : 1)) {
        if (m_Samples != (samples ? samples : 1))
            HEARTH_WARN("the swapchain asked for {}x MSAA; using {}x", samples, m_Samples);
        Build(width, height);
    }

    VulkanSwapchain::OwnedImage VulkanSwapchain::MakeImage(VkFormat format, VkImageUsageFlags usage,
                                                           VkImageAspectFlags aspect) {
        OwnedImage out;
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType   = VK_IMAGE_TYPE_2D;
        info.format      = format;
        info.extent      = { m_Width, m_Height, 1 };
        info.mipLevels   = 1;
        info.arrayLayers = 1;
        info.samples     = static_cast<VkSampleCountFlagBits>(m_Samples);
        info.tiling      = VK_IMAGE_TILING_OPTIMAL;
        info.usage       = usage;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        HEARTH_VK_CHECK(vmaCreateImage(m_Device.Allocator(), &info, &alloc,
                                       &out.image, &out.allocation, nullptr));
        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image    = out.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format   = format;
        view.subresourceRange = { aspect, 0, 1, 0, 1 };
        HEARTH_VK_CHECK(vkCreateImageView(m_Device.Raw(), &view, nullptr, &out.view));
        return out;
    }

    void VulkanSwapchain::DestroyImage(OwnedImage& image) {
        if (image.view)  vkDestroyImageView(m_Device.Raw(), image.view, nullptr);
        if (image.image) vmaDestroyImage(m_Device.Allocator(), image.image, image.allocation);
        image = {};
    }

    // Sized with the chain. The multisampled image is stored between passes, not transient:
    // a second pass that loads must find the first pass's samples there.
    void VulkanSwapchain::BuildAttachments() {
        if (m_Samples > 1)
            m_Msaa = MakeImage(m_Format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        if (m_DepthFormat != Format::Undefined)
            m_Depth = MakeImage(ToVk(m_DepthFormat), VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    VulkanSwapchain::~VulkanSwapchain() { Destroy(); }

    void VulkanSwapchain::Build(u32 width, u32 height) {
        // A minimized window reports zero. Keep the old chain (there is nothing to present to
        // anyway) and let the caller skip frames until a size comes back.
        if (width == 0 || height == 0) { m_Width = width; m_Height = height; return; }

        vkb::SwapchainBuilder builder{ m_Device.Physical(), m_Device.Raw(), m_Surface };

        // UNORM, not SRGB. An SRGB swapchain makes the hardware encode on write, which
        // double-encodes anything already authored in sRGB -- the common case for 2D art. A
        // renderer that wants linear blending asks for it in its own targets.
        auto result = builder
            .set_desired_format(VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM,
                                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
            .set_desired_present_mode(m_VSync ? VK_PRESENT_MODE_FIFO_KHR
                                              : VK_PRESENT_MODE_IMMEDIATE_KHR)
            .set_desired_extent(width, height)
            // The image is laid out the way the window is, and the compositor turns it if the
            // display is turned (a landscape app on a portrait phone). vk-bootstrap's default
            // here is the surface's currentTransform, which promises the frames arrive already
            // rotated -- and nothing here rotates them, so Android showed the landscape image
            // squeezed into the panel's portrait shape. Pre-rotating in the app is faster still,
            // but needs the extent swapped and the transform passed to the renderer.
            .set_pre_transform_flags(VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
            // Lets a frame be copied out of the swapchain: a screenshot, a thumbnail.
            .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                 | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
            .set_old_swapchain(m_Swapchain)
            .build();

        if (!result) {
            // Keep what we have rather than tearing down a working chain over a transient failure.
            HEARTH_ERROR("swapchain build failed: {}", result.error().message());
            return;
        }

        // Only now: the old chain had to stay alive for set_old_swapchain above.
        Destroy();

        vkb::Swapchain built = result.value();
        m_Swapchain = built.swapchain;
        m_Format    = built.image_format;
        m_Width     = built.extent.width;
        m_Height    = built.extent.height;
        m_Images    = built.get_images().value();
        m_Views     = built.get_image_views().value();

        BuildAttachments();

        m_RenderFinished.resize(m_Images.size());
        for (auto& semaphore : m_RenderFinished) {
            VkSemaphoreCreateInfo info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            HEARTH_VK_CHECK(vkCreateSemaphore(m_Device.Raw(), &info, nullptr, &semaphore));
        }

        HEARTH_TRACE("swapchain {}x{}, {} images", m_Width, m_Height, m_Images.size());
    }

    VkFramebuffer VulkanSwapchain::Framebuffer(u32 i) {
        if (m_Framebuffers.size() != m_Images.size()) m_Framebuffers.assign(m_Images.size(), VK_NULL_HANDLE);
        if (m_Framebuffers[i]) return m_Framebuffers[i];
        // Same order as RenderPassFor: colour, its resolve target, depth.
        std::vector<VkImageView> views;
        if (m_Msaa.view) { views.push_back(m_Msaa.view); views.push_back(m_Views[i]); }
        else             views.push_back(m_Views[i]);
        if (m_Depth.view) views.push_back(m_Depth.view);
        VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        info.renderPass = m_Device.RenderPassFor({ m_Format }, ToVk(m_DepthFormat), m_Samples,
                                                 LoadOp::Clear);
        info.attachmentCount = static_cast<u32>(views.size());
        info.pAttachments = views.data();
        info.width = m_Width;
        info.height = m_Height;
        info.layers = 1;
        HEARTH_VK_CHECK(vkCreateFramebuffer(m_Device.Raw(), &info, nullptr, &m_Framebuffers[i]));
        return m_Framebuffers[i];
    }

    void VulkanSwapchain::Destroy() {
        for (auto fb : m_Framebuffers) if (fb) vkDestroyFramebuffer(m_Device.Raw(), fb, nullptr);
        m_Framebuffers.clear();
        DestroyImage(m_Msaa);
        DestroyImage(m_Depth);
        if (!m_Swapchain && m_Views.empty()) return;
        for (auto view : m_Views) vkDestroyImageView(m_Device.Raw(), view, nullptr);
        for (auto semaphore : m_RenderFinished) vkDestroySemaphore(m_Device.Raw(), semaphore, nullptr);
        if (m_Swapchain) vkDestroySwapchainKHR(m_Device.Raw(), m_Swapchain, nullptr);
        m_Views.clear();
        m_Images.clear();
        m_RenderFinished.clear();
        m_Swapchain = VK_NULL_HANDLE;
    }

    void VulkanSwapchain::Resize(u32 width, u32 height) {
        if (width == m_Width && height == m_Height) return;
        m_Device.WaitIdle();
        Build(width, height);
    }

}
