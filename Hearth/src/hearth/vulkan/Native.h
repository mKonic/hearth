#pragma once

// You are leaving the abstraction. That is a supported thing to do.
//
// The failure mode this header exists to prevent: a renderer needs one Vulkan feature hearth does
// not wrap yet, and the only way forward is to add it to hearth, rebuild hearth, and land a
// two-repo change to ship one feature. That tax is how a shared layer stops being worth using.
// So every handle hearth owns is reachable, and reaching for one is a normal thing to do while
// the wrapper catches up -- not a fork.
//
// What you give up by using these: hearth makes no promise about the state of a handle it did not
// hand you back through its own API, and a layout it tracks internally (Texture::Layout) will be
// wrong if you transition an image behind its back. Record into CurrentCommandBuffer inside a
// frame and it is the same buffer hearth is recording into, in the same pass.

#include "hearth/Device.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace hearth::vk {

    struct Handles {
        VkInstance       instance = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice         device = VK_NULL_HANDLE;
        VkQueue          graphicsQueue = VK_NULL_HANDLE;
        u32              graphicsQueueFamily = 0;
        VmaAllocator     allocator = nullptr;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    };

    const Handles& Native(Device& device);

    // The command buffer of the frame currently open. VK_NULL_HANDLE outside BeginFrame/EndFrame
    // and inside SubmitImmediate -- the immediate path hands you its own through the CommandList.
    VkCommandBuffer CurrentCommandBuffer(Device& device);

    // Runs a one-shot command buffer and blocks until it retires. The raw form of
    // Device::SubmitImmediate, for work that is not a render pass: a manual layout transition, a
    // buffer-to-buffer copy, a query pool reset.
    void ImmediateSubmit(Device& device, const std::function<void(VkCommandBuffer)>& record);

    // Handles behind the resource interfaces. Each asserts the object came from this backend.
    VkBuffer        Raw(Buffer& buffer);
    VkImage         RawImage(Texture& texture);
    VkImageView     RawView(Texture& texture);
    VkSampler       RawSampler(Texture& texture);
    VkShaderModule  Raw(Shader& shader);
    VkPipeline      Raw(Pipeline& pipeline);
    VkPipelineLayout RawLayout(Pipeline& pipeline);
    VkDescriptorSet Raw(BindGroup& group);

    // A sync2 layout transition with conservative stage/access masks. Coarser than a hand-tuned
    // barrier and correct everywhere; three separate copies of exactly this function are what
    // started this library.
    void TransitionImage(VkCommandBuffer cmd, VkImage image,
                         VkImageLayout from, VkImageLayout to,
                         VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

    const char* ResultName(VkResult result);

}
