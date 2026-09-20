#pragma once

#include "hearth/Resources.h"
#include "platform/vulkan/VulkanCommon.h"
#include "platform/vulkan/VulkanDevice.h"

#include <string>
#include <vector>

namespace hearth {

    class VulkanDevice;

    class VulkanBuffer final : public Buffer {
    public:
        VulkanBuffer(VulkanDevice& device, const BufferDesc& desc);
        ~VulkanBuffer() override;

        void Upload(const void* data, u64 size, u64 offset = 0) override;
        void* Map() override { return m_Mapped; }
        u64 Size() const override { return m_Desc.size; }
        const BufferDesc& Desc() const override { return m_Desc; }

        VkBuffer Raw() const { return m_Buffer; }

    private:
        VulkanDevice& m_Device;
        BufferDesc    m_Desc;
        VkBuffer      m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = nullptr;
        void*         m_Mapped = nullptr;
    };

    class VulkanTexture final : public Texture {
    public:
        VulkanTexture(VulkanDevice& device, const TextureDesc& desc);
        // Wraps an image this object does not own (a swapchain image handed to a debug view).
        VulkanTexture(VulkanDevice& device, const TextureDesc& desc, VkImage image, VkImageView view);
        ~VulkanTexture() override;

        void Upload(const void* pixels, u64 size) override;
        void UploadRegion(const void* pixels, u32 x, u32 y, u32 w, u32 h) override;
        void UploadLayer(const void* pixels, u64 size, u32 layer) override;
        u32 Width()  const override { return m_Desc.width; }
        u32 Height() const override { return m_Desc.height; }
        u32 MipLevels() const override { return m_MipLevels; }
        const TextureDesc& Desc() const override { return m_Desc; }

        VkImage       Image()   const { return m_Image; }
        VkImageView   View()    const { return m_View; }
        VkSampler     Sampler() const { return m_Sampler; }
        VkImageLayout Layout()  const { return m_Layout; }
        void SetLayout(VkImageLayout layout) { m_Layout = layout; }

    private:
        // Copies `pixels` into `layer`, then rebuilds the mip chain for that layer if there
        // is one. Both upload paths funnel here.
        void UploadInto(const void* pixels, u32 x, u32 y, u32 w, u32 h, u32 layer);
        void GenerateMips(VkCommandBuffer cmd, u32 layer);

        VulkanDevice& m_Device;
        TextureDesc   m_Desc;
        u32           m_MipLevels = 1;
        VkImage       m_Image = VK_NULL_HANDLE;
        VkImageView   m_View = VK_NULL_HANDLE;
        // Owned by the device's sampler cache, not by this texture.
        VkSampler     m_Sampler = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = nullptr;
        VkImageLayout m_Layout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool          m_Owned = true;
    };

    class VulkanShader final : public Shader {
    public:
        VulkanShader(VulkanDevice& device, const ShaderDesc& desc);
        ~VulkanShader() override;

        ShaderStage Stage() const override { return m_Stage; }
        VkShaderModule Module() const { return m_Module; }
        const std::string& EntryPoint() const { return m_EntryPoint; }

    private:
        VulkanDevice&  m_Device;
        VkShaderModule m_Module = VK_NULL_HANDLE;
        ShaderStage    m_Stage;
        std::string    m_EntryPoint;
    };

    class VulkanPipeline final : public Pipeline {
    public:
        VulkanPipeline(VulkanDevice& device, const PipelineDesc& desc);
        VulkanPipeline(VulkanDevice& device, const ComputePipelineDesc& desc);
        ~VulkanPipeline() override;

        bool IsCompute() const override { return m_Compute; }
        const std::vector<BindingSlot>& Bindings() const override { return m_Bindings; }
        const std::string& DebugName() const override { return m_DebugName; }

        const PipelineDesc& Desc() const override {
            HEARTH_ASSERT(!m_Compute, "Desc() on the compute pipeline '{}': it has no vertex "
                                      "layout, blend state or attachment formats", m_DebugName);
            return m_Desc;
        }

        VkPipeline            Raw() const { return m_Pipeline; }
        VkPipelineBindPoint   BindPoint() const {
            return m_Compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
        }
        VkPipelineLayout      Layout() const { return m_Layout; }
        VkDescriptorSetLayout SetLayout() const { return m_SetLayout; }
        bool HasBindings() const { return m_SetLayout != VK_NULL_HANDLE; }
        u32  PushConstantSize() const { return m_PushConstantSize; }

    private:
        // Both constructors funnel into this: the descriptor set layout, the pipeline layout
        // and the push-constant range are identical for graphics and compute.
        void BuildLayout(const std::vector<BindingSlot>& bindings, u32 pushConstantSize);

        VulkanDevice&         m_Device;
        bool                  m_Compute = false;
        std::vector<BindingSlot> m_Bindings;
        std::string           m_DebugName;
        u32                   m_PushConstantSize = 0;
        PipelineDesc          m_Desc;
        VkPipeline            m_Pipeline = VK_NULL_HANDLE;
        VkPipelineLayout      m_Layout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_SetLayout = VK_NULL_HANDLE;
    };

    class VulkanBindGroup final : public BindGroup {
    public:
        VulkanBindGroup(VulkanDevice& device, const Ref<Pipeline>& pipeline,
                        const std::vector<BindGroupEntry>& entries);
        ~VulkanBindGroup() override;

        void Update(const std::vector<BindGroupEntry>& entries) override;

        VkDescriptorSet  Raw() const { return m_Set; }
        VkPipelineLayout Layout() const { return m_PipelineLayout; }

    private:
        VulkanDevice&    m_Device;
        Ref<Pipeline>    m_Pipeline;      // keeps the set layout alive for as long as the set
        VulkanDevice::DescriptorAllocation m_Allocation{};
        VkDescriptorSet  m_Set = VK_NULL_HANDLE;
        VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    };

    class VulkanRenderTarget final : public RenderTarget {
    public:
        VulkanRenderTarget(VulkanDevice& device, const RenderTargetDesc& desc);
        ~VulkanRenderTarget() override;

        u32 Width()  const override { return m_Desc.width; }
        u32 Height() const override { return m_Desc.height; }
        u32 ColorAttachmentCount() const override { return static_cast<u32>(m_Color.size()); }
        Ref<Texture> ColorTexture(u32 index = 0) const override;
        void Resize(u32 width, u32 height) override;
        void ReadPixels(void* outPixels, u64 size, u32 index = 0) override;

        // The view a render pass writes into: the multisampled image when this target is
        // multisampled, otherwise the resolve image itself.
        VkImageView ColorView(u32 index) const;
        // Where a multisampled attachment resolves to. VK_NULL_HANDLE when samples == 1.
        VkImageView ResolveView(u32 index) const;
        // The image a render pass actually writes: the multisampled one when there is one.
        VkImage ColorImage(u32 index) const;
        VkImageView DepthView() const { return m_DepthView; }
        VkSampleCountFlagBits Samples() const { return m_Samples; }
        const RenderTargetDesc& Desc() const { return m_Desc; }

    private:
        void Build();
        void Destroy();

        // One colour attachment: the sampleable image, plus the multisampled one that
        // resolves into it when this target is multisampled.
        struct Attachment {
            Ref<VulkanTexture> resolve;
            VkImage       msaaImage = VK_NULL_HANDLE;
            VkImageView   msaaView = VK_NULL_HANDLE;
            VmaAllocation msaaAllocation = nullptr;
        };

        VulkanDevice&           m_Device;
        RenderTargetDesc        m_Desc;
        std::vector<Attachment> m_Color;
        VkSampleCountFlagBits   m_Samples = VK_SAMPLE_COUNT_1_BIT;
        VkImage            m_DepthImage = VK_NULL_HANDLE;
        VkImageView        m_DepthView = VK_NULL_HANDLE;
        VmaAllocation      m_DepthAllocation = nullptr;
    };

}
