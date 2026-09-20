#pragma once

#include "hearth/Types.h"
#include "hearth/Log.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace hearth {

    const char* VkResultName(VkResult r);

    // Every Vulkan call that can fail goes through this. A silent VK_ERROR_DEVICE_LOST that only
    // surfaces three frames later as garbage on screen is the worst class of bug in this layer.
    #define HEARTH_VK_CHECK(expr)                                                                 \
        do {                                                                                      \
            const VkResult hearthVkResult = (expr);                                               \
            if (hearthVkResult != VK_SUCCESS) [[unlikely]]                                        \
                HEARTH_ASSERT(false, "{} failed: {}", #expr,                                      \
                              ::hearth::VkResultName(hearthVkResult));                            \
        } while (0)

    VkFormat              ToVk(Format f);
    Format                FromVk(VkFormat f);
    VkFilter              ToVk(Filter f);
    VkSamplerAddressMode  ToVk(AddressMode m);
    VkPrimitiveTopology   ToVk(PrimitiveTopology t);
    VkShaderStageFlagBits ToVk(ShaderStage s);
    VkShaderStageFlags    ToVkStageFlags(u8 stageBits);
    VkDescriptorType      ToVk(BindingType t);
    VkIndexType           ToVk(IndexType t);

    VkPipelineColorBlendAttachmentState BlendState(BlendMode mode);

    void TransitionImageRaw(VkCommandBuffer cmd, VkImage image,
                            VkImageLayout from, VkImageLayout to,
                            VkImageAspectFlags aspect);

}
