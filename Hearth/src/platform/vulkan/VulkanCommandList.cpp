#include "platform/vulkan/VulkanCommandList.h"

#include "platform/vulkan/VulkanDevice.h"
#include "platform/vulkan/VulkanResources.h"
#include "platform/vulkan/VulkanSwapchain.h"

namespace hearth {

    void VulkanCommandList::Begin(VkCommandBuffer cmd, u32 swapchainImageIndex, bool offscreenOnly) {
        m_Cmd = cmd;
        m_ImageIndex = swapchainImageIndex;
        m_BoundPipeline = nullptr;
        m_CurrentTarget = nullptr;
        m_InPass = false;
        m_TouchedSwapchain = false;
        m_OffscreenOnly = offscreenOnly;
    }

    void VulkanCommandList::End() {
        HEARTH_ASSERT(!m_InPass, "the command list ended with a render pass still open");
        m_Cmd = VK_NULL_HANDLE;
    }

    void VulkanCommandList::Barrier(VkImage image, VkImageLayout from, VkImageLayout to,
                                    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                                    VkImageAspectFlags aspect) {
        VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = { aspect, 0, 1, 0, 1 };

        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(m_Cmd, &dep);
    }

    void VulkanCommandList::BeginRenderPass(const RenderPassDesc& desc) {
        HEARTH_ASSERT(!m_InPass, "BeginRenderPass while a pass is already open");
        HEARTH_ASSERT(!(m_OffscreenOnly && !desc.target),
                      "a render pass recorded through SubmitImmediate must name a target: there "
                      "is no acquired swapchain image outside the frame loop");
        m_InPass = true;
        m_CurrentTarget = static_cast<VulkanRenderTarget*>(desc.target);

        VkImageView colorView = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        u32 width = 0, height = 0;

        if (m_CurrentTarget) {
            auto* color = static_cast<VulkanTexture*>(m_CurrentTarget->ColorTexture().get());
            Barrier(color->Image(), color->Layout(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            color->SetLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            colorView = m_CurrentTarget->ColorView();
            depthView = m_CurrentTarget->DepthView();
            width     = m_CurrentTarget->Width();
            height    = m_CurrentTarget->Height();
        } else {
            auto* swapchain = m_Device.SwapchainImpl();
            HEARTH_ASSERT(swapchain && swapchain->Valid(),
                          "no swapchain to render into: this device was created headless, so "
                          "every render pass must name a target");

            if (!m_TouchedSwapchain) {
                Barrier(swapchain->Image(m_ImageIndex), VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                m_TouchedSwapchain = true;
            }
            colorView = swapchain->View(m_ImageIndex);
            width     = swapchain->Width();
            height    = swapchain->Height();
        }

        VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        color.imageView   = colorView;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp      = desc.loadOp == LoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                          : desc.loadOp == LoadOp::Load  ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                         : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = { { desc.clearColor.r, desc.clearColor.g,
                                     desc.clearColor.b, desc.clearColor.a } };

        VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        depth.imageView   = depthView;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = { desc.clearDepth, 0 };

        VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        info.renderArea = { {0, 0}, { width, height } };
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        info.pDepthAttachment = depthView ? &depth : nullptr;

        vkCmdBeginRendering(m_Cmd, &info);

        // NOT a negative-height viewport. Vulkan's clip space already has +y pointing down; the
        // y-flip that ports of OpenGL renderers carry would invert everything drawn here. A
        // renderer that wants y-up bakes it into its projection matrix, where it is visible.
        VkViewport vp{ 0.0f, 0.0f, static_cast<f32>(width), static_cast<f32>(height), 0.0f, 1.0f };
        vkCmdSetViewport(m_Cmd, 0, 1, &vp);
        VkRect2D scissor{ {0, 0}, { width, height } };
        vkCmdSetScissor(m_Cmd, 0, 1, &scissor);
    }

    void VulkanCommandList::EndRenderPass() {
        HEARTH_ASSERT(m_InPass, "EndRenderPass without an open pass");
        vkCmdEndRendering(m_Cmd);
        m_InPass = false;

        if (m_CurrentTarget) {
            // Leave an offscreen target sampleable: the next thing that touches it is almost
            // always a shader reading it back, or a readback copy.
            auto* color = static_cast<VulkanTexture*>(m_CurrentTarget->ColorTexture().get());
            Barrier(color->Image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            color->SetLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        m_CurrentTarget = nullptr;
        m_BoundPipeline = nullptr;
    }

    void VulkanCommandList::BindPipeline(const Ref<Pipeline>& pipeline) {
        HEARTH_ASSERT(pipeline, "BindPipeline with a null pipeline");
        m_BoundPipeline = static_cast<VulkanPipeline*>(pipeline.get());
        HEARTH_ASSERT(!(m_BoundPipeline->IsCompute() && m_InPass),
                      "compute pipeline '{}' bound inside a render pass",
                      m_BoundPipeline->DebugName());
        vkCmdBindPipeline(m_Cmd, m_BoundPipeline->BindPoint(), m_BoundPipeline->Raw());
    }

    void VulkanCommandList::BindBindGroup(const Ref<BindGroup>& group) {
        HEARTH_ASSERT(group, "BindBindGroup with a null group");
        HEARTH_ASSERT(m_BoundPipeline, "BindBindGroup before a pipeline is bound: the bind "
                                       "point comes from the pipeline");
        auto* g = static_cast<VulkanBindGroup*>(group.get());
        VkDescriptorSet set = g->Raw();
        vkCmdBindDescriptorSets(m_Cmd, m_BoundPipeline->BindPoint(), g->Layout(),
                                0, 1, &set, 0, nullptr);
    }

    void VulkanCommandList::BindVertexBuffer(u32 slot, const Ref<Buffer>& buffer, u64 offset) {
        HEARTH_ASSERT(buffer, "BindVertexBuffer with a null buffer");
        VkBuffer raw = static_cast<VulkanBuffer*>(buffer.get())->Raw();
        vkCmdBindVertexBuffers(m_Cmd, slot, 1, &raw, &offset);
    }

    void VulkanCommandList::BindIndexBuffer(const Ref<Buffer>& buffer, IndexType type, u64 offset) {
        HEARTH_ASSERT(buffer, "BindIndexBuffer with a null buffer");
        vkCmdBindIndexBuffer(m_Cmd, static_cast<VulkanBuffer*>(buffer.get())->Raw(),
                             offset, ToVk(type));
    }

    void VulkanCommandList::SetViewport(const Viewport& vp) {
        VkViewport v{ vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth };
        vkCmdSetViewport(m_Cmd, 0, 1, &v);
    }

    void VulkanCommandList::SetScissor(const ScissorRect& rect) {
        VkRect2D r{ { rect.x, rect.y }, { rect.width, rect.height } };
        vkCmdSetScissor(m_Cmd, 0, 1, &r);
    }

    void VulkanCommandList::SetPushConstants(const void* data, u32 size) {
        HEARTH_ASSERT(m_BoundPipeline, "push constants before a pipeline is bound");
        HEARTH_ASSERT(size <= m_BoundPipeline->PushConstantSize(),
                      "{} bytes of push constants into pipeline '{}', which reserved {}",
                      size, m_BoundPipeline->DebugName(),
                      m_BoundPipeline->PushConstantSize());
        vkCmdPushConstants(m_Cmd, m_BoundPipeline->Layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                               | VK_SHADER_STAGE_COMPUTE_BIT,
                           0, size, data);
    }

    void VulkanCommandList::Draw(u32 vertexCount, u32 instanceCount,
                                 u32 firstVertex, u32 firstInstance) {
        HEARTH_ASSERT(m_BoundPipeline, "Draw before a pipeline is bound");
        vkCmdDraw(m_Cmd, vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void VulkanCommandList::DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                                        i32 vertexOffset, u32 firstInstance) {
        HEARTH_ASSERT(m_BoundPipeline, "DrawIndexed before a pipeline is bound");
        vkCmdDrawIndexed(m_Cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void VulkanCommandList::Dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) {
        HEARTH_ASSERT(m_BoundPipeline, "Dispatch before a pipeline is bound");
        HEARTH_ASSERT(m_BoundPipeline->IsCompute(),
                      "Dispatch with the graphics pipeline '{}' bound",
                      m_BoundPipeline->DebugName());
        HEARTH_ASSERT(!m_InPass, "Dispatch inside a render pass");
        HEARTH_ASSERT(groupsX > 0 && groupsY > 0 && groupsZ > 0,
                      "Dispatch({}, {}, {}) does nothing -- a zero group count is almost "
                      "always a division that rounded down", groupsX, groupsY, groupsZ);
        vkCmdDispatch(m_Cmd, groupsX, groupsY, groupsZ);
    }

    // ALL_COMMANDS both sides, every access bit. A caller reaching for this is pairing a
    // compute dispatch with the draw that reads its output, and the cost of one over-wide
    // barrier is a pipeline bubble -- the cost of a too-narrow one is a result that is right
    // on the machine it was written on. hearth/vulkan/Native.h is there for anyone who has
    // measured this and wants the precise masks.
    void VulkanCommandList::MemoryBarrier() {
        VkMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(m_Cmd, &dep);
    }

}
