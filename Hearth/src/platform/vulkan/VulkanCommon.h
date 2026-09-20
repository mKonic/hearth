#pragma once

#include "hearth/Types.h"
#include "hearth/Log.h"

#include <string>

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

    // Attaches a debugName to a Vulkan handle, so validation messages, RenderDoc captures and
    // device-lost reports name the object instead of printing a number. A no-op when the
    // debug-utils extension is absent, which is the normal case in a shipped build -- so this
    // costs nothing there and is never worth guarding at the call site.
    void SetObjectName(VkDevice device, VkObjectType type, u64 handle, const std::string& name);

    // One overload per handle type, so a caller cannot pair the wrong VkObjectType
    // with a handle -- the compiler picks the pairing.
    inline void SetObjectName(VkDevice device, VkBuffer handle, const std::string& name) {
        SetObjectName(device, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(handle), name);
    }

    inline void SetObjectName(VkDevice device, VkImage handle, const std::string& name) {
        SetObjectName(device, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(handle), name);
    }

    inline void SetObjectName(VkDevice device, VkImageView handle, const std::string& name) {
        SetObjectName(device, VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<u64>(handle), name);
    }

    inline void SetObjectName(VkDevice device, VkShaderModule handle, const std::string& name) {
        SetObjectName(device, VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<u64>(handle), name);
    }

    inline void SetObjectName(VkDevice device, VkPipeline handle, const std::string& name) {
        SetObjectName(device, VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(handle), name);
    }

    inline void SetObjectName(VkDevice device, VkPipelineLayout handle, const std::string& name) {
        SetObjectName(device, VK_OBJECT_TYPE_PIPELINE_LAYOUT, reinterpret_cast<u64>(handle), name);
    }

    inline void SetObjectName(VkDevice device, VkDescriptorSet handle, const std::string& name) {
        SetObjectName(device, VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<u64>(handle), name);
    }

    inline void SetObjectName(VkDevice device, VkSampler handle, const std::string& name) {
        SetObjectName(device, VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<u64>(handle), name);
    }

}
