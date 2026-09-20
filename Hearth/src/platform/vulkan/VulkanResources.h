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
        u32 Width()  const override { return m_Desc.width; }
        u32 Height() const override { return m_Desc.height; }
        const TextureDesc& Desc() const override { return m_Desc; }

        VkImage       Image()   const { return m_Image; }
        VkImageView   View()    const { return m_View; }
        VkSampler     Sampler() const { return m_Sampler; }
        VkImageLayout Layout()  const { return m_Layout; }
        void SetLayout(VkImageLayout layout) { m_Layout = layout; }

    private:
        VulkanDevice& m_Device;
        TextureDesc   m_Desc;
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
        Ref<Texture> ColorTexture() const override { return m_Color; }
        void Resize(u32 width, u32 height) override;
        void ReadPixels(void* outPixels, u64 size) override;

        VkImageView ColorView() const;
        VkImageView DepthView() const { return m_DepthView; }
        const RenderTargetDesc& Desc() const { return m_Desc; }

    private:
        void Build();
        void Destroy();

        VulkanDevice&      m_Device;
        RenderTargetDesc   m_Desc;
        Ref<VulkanTexture> m_Color;
        VkImage            m_DepthImage = VK_NULL_HANDLE;
        VkImageView        m_DepthView = VK_NULL_HANDLE;
        VmaAllocation      m_DepthAllocation = nullptr;
    };

}
