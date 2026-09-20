#pragma once

#include "hearth/CommandList.h"
#include "platform/vulkan/VulkanCommon.h"

namespace hearth {

    class VulkanDevice;
    class VulkanPipeline;
    class VulkanRenderTarget;

    class VulkanCommandList final : public CommandList {
    public:
        explicit VulkanCommandList(VulkanDevice& device) : m_Device(device) {}

        // offscreenOnly marks a list recorded outside the frame loop (Device::SubmitImmediate).
        // There is no acquired swapchain image to default a render pass to, so naming no target
        // is a mistake worth catching here rather than as a null image view at draw time.
        void Begin(VkCommandBuffer cmd, u32 swapchainImageIndex, bool offscreenOnly);
        void End();

        void BeginRenderPass(const RenderPassDesc& desc) override;
        void EndRenderPass() override;

        void BindPipeline(const Ref<Pipeline>& pipeline) override;
        void BindBindGroup(const Ref<BindGroup>& group) override;
        void BindVertexBuffer(u32 slot, const Ref<Buffer>& buffer, u64 offset = 0) override;
        void BindIndexBuffer(const Ref<Buffer>& buffer, IndexType type = IndexType::U32,
                             u64 offset = 0) override;

        void SetViewport(const Viewport& vp) override;
        void SetScissor(const ScissorRect& rect) override;
        void SetPushConstants(const void* data, u32 size) override;

        void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override;
        void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                         i32 vertexOffset, u32 firstInstance) override;
        void Dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) override;
        void MemoryBarrier() override;

        VkCommandBuffer Raw() const { return m_Cmd; }
        bool TouchedSwapchain() const { return m_TouchedSwapchain; }

    private:
        void Barrier(VkImage image, VkImageLayout from, VkImageLayout to,
                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                     VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                     VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

        VulkanDevice&   m_Device;
        VkCommandBuffer m_Cmd = VK_NULL_HANDLE;
        u32             m_ImageIndex = 0;

        VulkanPipeline*     m_BoundPipeline = nullptr;
        VulkanRenderTarget* m_CurrentTarget = nullptr;
        bool m_InPass = false;
        bool m_TouchedSwapchain = false;
        bool m_OffscreenOnly = false;
    };

}
