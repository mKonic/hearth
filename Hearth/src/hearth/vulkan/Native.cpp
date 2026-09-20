#include "hearth/vulkan/Native.h"

#include "platform/vulkan/VulkanCommandList.h"
#include "platform/vulkan/VulkanDevice.h"
#include "platform/vulkan/VulkanResources.h"

namespace hearth::vk {

    namespace {

        VulkanDevice& Impl(Device& device) { return static_cast<VulkanDevice&>(device); }

        // Rebuilt on each call rather than cached: a device that has been through OnSurfaceLost
        // and back is the same set of handles, but a device that failed to initialise is not, and
        // a stale struct would hand out handles that were destroyed.
        thread_local Handles t_Handles{};

    }

    const Handles& Native(Device& device) {
        auto& impl = Impl(device);
        t_Handles.instance            = impl.Instance();
        t_Handles.physicalDevice      = impl.Physical();
        t_Handles.device              = impl.Raw();
        t_Handles.graphicsQueue       = impl.GraphicsQueue();
        t_Handles.graphicsQueueFamily = impl.GraphicsQueueFamily();
        t_Handles.allocator           = impl.Allocator();
        t_Handles.descriptorPool      = impl.DescriptorPool();
        return t_Handles;
    }

    VkCommandBuffer CurrentCommandBuffer(Device& device) {
        return Impl(device).CurrentCommandBuffer();
    }

    void ImmediateSubmit(Device& device, const std::function<void(VkCommandBuffer)>& record) {
        Impl(device).ImmediateSubmitRaw(record);
    }

    VkBuffer         Raw(Buffer& buffer)         { return static_cast<VulkanBuffer&>(buffer).Raw(); }
    VkImage          RawImage(Texture& texture)  { return static_cast<VulkanTexture&>(texture).Image(); }
    VkImageView      RawView(Texture& texture)   { return static_cast<VulkanTexture&>(texture).View(); }
    VkSampler        RawSampler(Texture& texture){ return static_cast<VulkanTexture&>(texture).Sampler(); }
    VkShaderModule   Raw(Shader& shader)         { return static_cast<VulkanShader&>(shader).Module(); }
    VkPipeline       Raw(Pipeline& pipeline)     { return static_cast<VulkanPipeline&>(pipeline).Raw(); }
    VkPipelineLayout RawLayout(Pipeline& pipeline){ return static_cast<VulkanPipeline&>(pipeline).Layout(); }
    VkDescriptorSet  Raw(BindGroup& group)       { return static_cast<VulkanBindGroup&>(group).Raw(); }

    void TransitionImage(VkCommandBuffer cmd, VkImage image,
                         VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect) {
        TransitionImageRaw(cmd, image, from, to, aspect);
    }

    const char* ResultName(VkResult result) { return VkResultName(result); }

}
