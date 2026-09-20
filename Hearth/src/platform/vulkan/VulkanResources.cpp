#include "platform/vulkan/VulkanResources.h"

#include "platform/vulkan/VulkanDevice.h"

#include <algorithm>
#include <cstring>

namespace hearth {

    namespace {

        VkBufferUsageFlags UsageFlags(BufferUsage u) {
            switch (u) {
                case BufferUsage::Vertex:  return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT  | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                case BufferUsage::Index:   return VK_BUFFER_USAGE_INDEX_BUFFER_BIT   | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                case BufferUsage::Uniform: return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                case BufferUsage::Storage: return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                case BufferUsage::Staging: return VK_BUFFER_USAGE_TRANSFER_SRC_BIT   | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            }
            return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        }

    }

    // ---------------------------------------------------------------------------- Buffer

    VulkanBuffer::VulkanBuffer(VulkanDevice& device, const BufferDesc& desc)
        : m_Device(device), m_Desc(desc) {
        HEARTH_ASSERT(desc.size > 0, "zero-sized buffer '{}'", desc.debugName);

        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size  = desc.size;
        info.usage = UsageFlags(desc.usage);

        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        if (desc.memory == MemoryKind::HostVisible) {
            // Persistently mapped and sequentially written: this is the per-frame batch buffer
            // path, and re-mapping it every frame would be pure overhead.
            alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                        | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }

        VmaAllocationInfo allocInfo{};
        HEARTH_VK_CHECK(vmaCreateBuffer(m_Device.Allocator(), &info, &alloc,
                                        &m_Buffer, &m_Allocation, &allocInfo));
        m_Mapped = allocInfo.pMappedData;
        SetObjectName(m_Device.Raw(), m_Buffer, desc.debugName);
    }

    VulkanBuffer::~VulkanBuffer() {
        if (m_Buffer) vmaDestroyBuffer(m_Device.Allocator(), m_Buffer, m_Allocation);
    }

    void VulkanBuffer::Upload(const void* data, u64 size, u64 offset) {
        HEARTH_ASSERT(offset + size <= m_Desc.size,
                      "upload of {} bytes at offset {} overruns the {}-byte buffer '{}'",
                      size, offset, m_Desc.size, m_Desc.debugName);

        if (m_Mapped) {
            std::memcpy(static_cast<u8*>(m_Mapped) + offset, data, size);
            return;
        }

        // Device-local: stage and copy.
        VulkanBuffer staging(m_Device, BufferDesc{ size, BufferUsage::Staging,
                                                   MemoryKind::HostVisible, "staging" });
        std::memcpy(staging.Map(), data, size);

        m_Device.ImmediateSubmitRaw([&](VkCommandBuffer cmd) {
            VkBufferCopy copy{ 0, offset, size };
            vkCmdCopyBuffer(cmd, staging.Raw(), m_Buffer, 1, &copy);
        });
    }

    // ---------------------------------------------------------------------------- Texture

    VulkanTexture::VulkanTexture(VulkanDevice& device, const TextureDesc& desc)
        : m_Device(device), m_Desc(desc) {
        const bool depth  = IsDepthFormat(desc.format);
        const bool target = desc.usage != TextureUsage::Sampled;

        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType   = VK_IMAGE_TYPE_2D;
        info.format      = ToVk(desc.format);
        info.extent      = { desc.width, desc.height, 1 };
        info.mipLevels   = 1;
        info.arrayLayers = 1;
        info.samples     = VK_SAMPLE_COUNT_1_BIT;
        info.tiling      = VK_IMAGE_TILING_OPTIMAL;
        info.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (desc.usage == TextureUsage::Storage) info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (target && desc.usage != TextureUsage::Storage) {
            info.usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            // So RenderTarget::ReadPixels can copy the result out. Without it the image is
            // renderable and unreadable, which is a confusing place to discover at capture time.
            if (!depth) info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        HEARTH_VK_CHECK(vmaCreateImage(m_Device.Allocator(), &info, &alloc,
                                       &m_Image, &m_Allocation, nullptr));

        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image    = m_Image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format   = info.format;
        view.subresourceRange = { static_cast<VkImageAspectFlags>(
                                      depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                                  0, 1, 0, 1 };
        HEARTH_VK_CHECK(vkCreateImageView(m_Device.Raw(), &view, nullptr, &m_View));

        if (!depth)
            m_Sampler = m_Device.SamplerFor(desc.minFilter, desc.magFilter, desc.addressMode);

        if (desc.usage == TextureUsage::Storage) {
            // Moved to GENERAL once, here, and left there. A storage image has to be in
            // GENERAL to be written, and an image that is written by compute and sampled by
            // graphics cannot be in two layouts at once -- so hearth picks the one that
            // permits both rather than transitioning around every use.
            m_Device.ImmediateSubmitRaw([&](VkCommandBuffer cmd) {
                TransitionImageRaw(cmd, m_Image, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
            });
            m_Layout = VK_IMAGE_LAYOUT_GENERAL;
        }

        SetObjectName(m_Device.Raw(), m_Image, desc.debugName);
        SetObjectName(m_Device.Raw(), m_View, desc.debugName + ".view");
    }

    VulkanTexture::VulkanTexture(VulkanDevice& device, const TextureDesc& desc,
                                 VkImage image, VkImageView view)
        : m_Device(device), m_Desc(desc), m_Image(image), m_View(view), m_Owned(false) {
        if (!IsDepthFormat(desc.format))
            m_Sampler = m_Device.SamplerFor(desc.minFilter, desc.magFilter, desc.addressMode);
    }

    VulkanTexture::~VulkanTexture() {
        // m_Sampler belongs to the device's cache and outlives this texture on purpose.
        if (!m_Owned) return;
        if (m_View)  vkDestroyImageView(m_Device.Raw(), m_View, nullptr);
        if (m_Image) vmaDestroyImage(m_Device.Allocator(), m_Image, m_Allocation);
    }

    void VulkanTexture::Upload(const void* pixels, u64 size) {
        const u64 expected = u64(m_Desc.width) * m_Desc.height * FormatSize(m_Desc.format);
        HEARTH_ASSERT(size >= expected,
                      "upload of {} bytes is short of the {} bytes {}x{} {} needs",
                      size, expected, m_Desc.width, m_Desc.height, FormatName(m_Desc.format));
        UploadRegion(pixels, 0, 0, m_Desc.width, m_Desc.height);
    }

    void VulkanTexture::UploadRegion(const void* pixels, u32 x, u32 y, u32 w, u32 h) {
        HEARTH_ASSERT(x + w <= m_Desc.width && y + h <= m_Desc.height,
                      "region {}x{} at ({},{}) falls outside the {}x{} texture '{}'",
                      w, h, x, y, m_Desc.width, m_Desc.height, m_Desc.debugName);

        const u64 bytes = u64(w) * h * FormatSize(m_Desc.format);
        VulkanBuffer staging(m_Device, BufferDesc{ bytes, BufferUsage::Staging,
                                                   MemoryKind::HostVisible, "texture-staging" });
        std::memcpy(staging.Map(), pixels, bytes);

        const VkImageLayout from = m_Layout;
        // Where the image goes back to afterwards: a storage image lives in GENERAL, anything
        // else is left ready to sample.
        const VkImageLayout settled = m_Desc.usage == TextureUsage::Storage
                                    ? VK_IMAGE_LAYOUT_GENERAL
                                    : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        m_Device.ImmediateSubmitRaw([&](VkCommandBuffer cmd) {
            TransitionImageRaw(cmd, m_Image, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_ASPECT_COLOR_BIT);

            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageOffset = { static_cast<i32>(x), static_cast<i32>(y), 0 };
            copy.imageExtent = { w, h, 1 };
            vkCmdCopyBufferToImage(cmd, staging.Raw(), m_Image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            TransitionImageRaw(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, settled,
                               VK_IMAGE_ASPECT_COLOR_BIT);
        });

        m_Layout = settled;
    }

    // ---------------------------------------------------------------------------- Shader

    VulkanShader::VulkanShader(VulkanDevice& device, const ShaderDesc& desc)
        : m_Device(device), m_Stage(desc.stage), m_EntryPoint(desc.entryPoint) {
        HEARTH_ASSERT(!desc.spirv.empty(), "shader '{}' has no SPIR-V", desc.debugName);

        VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        info.codeSize = desc.spirv.size() * sizeof(u32);
        info.pCode    = desc.spirv.data();
        HEARTH_VK_CHECK(vkCreateShaderModule(m_Device.Raw(), &info, nullptr, &m_Module));
        SetObjectName(m_Device.Raw(), m_Module, desc.debugName);
    }

    VulkanShader::~VulkanShader() {
        if (m_Module) vkDestroyShaderModule(m_Device.Raw(), m_Module, nullptr);
    }

    // ---------------------------------------------------------------------------- Pipeline

    void VulkanPipeline::BuildLayout(const std::vector<BindingSlot>& bindings,
                                     u32 pushConstantSize) {
        if (!bindings.empty()) {
            std::vector<VkDescriptorSetLayoutBinding> vkBindings;
            std::vector<VkDescriptorBindingFlags> flags;
            vkBindings.reserve(bindings.size());
            flags.reserve(bindings.size());

            for (const auto& b : bindings) {
                vkBindings.push_back({ b.binding, ToVk(b.type), b.count,
                                       ToVkStageFlags(b.stages), nullptr });
                // An array binding may be left half-written, so PARTIALLY_BOUND always.
                // UPDATE_AFTER_BIND -- which is what lets a renderer that discovers a texture
                // mid-scene rewrite the array without waiting for every frame using it to
                // retire -- is added only for texture arrays: it is gated on a per-descriptor-
                // type device feature, and the uniform-buffer variant is missing on enough
                // hardware that requiring it would narrow which GPUs hearth runs on.
                VkDescriptorBindingFlags bindingFlags = 0;
                if (b.count > 1) {
                    bindingFlags |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
                    if (b.type == BindingType::SampledTexture)
                        bindingFlags |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
                }
                flags.push_back(bindingFlags);
            }

            const bool updateAfterBind =
                std::any_of(flags.begin(), flags.end(), [](VkDescriptorBindingFlags f) {
                    return (f & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) != 0;
                });

            VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            flagsInfo.bindingCount  = static_cast<u32>(flags.size());
            flagsInfo.pBindingFlags = flags.data();

            VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            info.pNext        = &flagsInfo;
            info.flags        = updateAfterBind
                              ? VkDescriptorSetLayoutCreateFlags{
                                    VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT }
                              : VkDescriptorSetLayoutCreateFlags{ 0 };
            info.bindingCount = static_cast<u32>(vkBindings.size());
            info.pBindings    = vkBindings.data();
            HEARTH_VK_CHECK(vkCreateDescriptorSetLayout(m_Device.Raw(), &info, nullptr, &m_SetLayout));
        }

        VkPushConstantRange push{};
        // Every stage, so one range serves both pipeline kinds and a caller never has to say
        // which stages will read it.
        push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                        | VK_SHADER_STAGE_COMPUTE_BIT;
        push.size       = pushConstantSize;

        VkPipelineLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout.setLayoutCount = m_SetLayout ? 1u : 0u;
        layout.pSetLayouts    = m_SetLayout ? &m_SetLayout : nullptr;
        layout.pushConstantRangeCount = pushConstantSize ? 1u : 0u;
        layout.pPushConstantRanges    = pushConstantSize ? &push : nullptr;
        HEARTH_VK_CHECK(vkCreatePipelineLayout(m_Device.Raw(), &layout, nullptr, &m_Layout));

        SetObjectName(m_Device.Raw(), m_Layout, m_DebugName + ".layout");
    }

    VulkanPipeline::VulkanPipeline(VulkanDevice& device, const ComputePipelineDesc& desc)
        : m_Device(device), m_Compute(true), m_Bindings(desc.bindings),
          m_DebugName(desc.debugName), m_PushConstantSize(desc.pushConstantSize) {
        auto* shader = static_cast<VulkanShader*>(desc.compute.get());
        HEARTH_ASSERT(shader, "compute pipeline '{}' has no shader", desc.debugName);
        HEARTH_ASSERT(shader->Stage() == ShaderStage::Compute,
                      "compute pipeline '{}' was given a {} shader", desc.debugName,
                      shader->Stage() == ShaderStage::Vertex ? "vertex" : "fragment");

        BuildLayout(desc.bindings, desc.pushConstantSize);

        VkPipelineShaderStageCreateInfo stage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader->Module();
        stage.pName  = shader->EntryPoint().c_str();

        VkComputePipelineCreateInfo info{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        info.stage  = stage;
        info.layout = m_Layout;
        HEARTH_VK_CHECK(vkCreateComputePipelines(m_Device.Raw(), m_Device.PipelineCache(), 1,
                                                 &info, nullptr, &m_Pipeline));
        SetObjectName(m_Device.Raw(), m_Pipeline, desc.debugName);
    }

    VulkanPipeline::VulkanPipeline(VulkanDevice& device, const PipelineDesc& desc)
        : m_Device(device), m_Bindings(desc.bindings), m_DebugName(desc.debugName),
          m_PushConstantSize(desc.pushConstantSize), m_Desc(desc) {

        BuildLayout(desc.bindings, desc.pushConstantSize);

        auto* vs = static_cast<VulkanShader*>(desc.vertex.get());
        auto* fs = static_cast<VulkanShader*>(desc.fragment.get());
        HEARTH_ASSERT(vs && fs, "pipeline '{}' needs both a vertex and a fragment shader",
                      desc.debugName);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs->Module();
        stages[0].pName  = vs->EntryPoint().c_str();
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs->Module();
        stages[1].pName  = fs->EntryPoint().c_str();

        std::vector<VkVertexInputBindingDescription> vertexBindings;
        std::vector<VkVertexInputAttributeDescription> vertexAttrs;
        for (u32 i = 0; i < desc.vertexBuffers.size(); ++i) {
            const auto& vb = desc.vertexBuffers[i];
            vertexBindings.push_back({ i, vb.stride,
                                       vb.stepMode == VertexStepMode::Instance
                                           ? VK_VERTEX_INPUT_RATE_INSTANCE
                                           : VK_VERTEX_INPUT_RATE_VERTEX });
            for (const auto& a : vb.attributes)
                vertexAttrs.push_back({ a.location, i, ToVk(a.format), a.offset });
        }

        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount   = static_cast<u32>(vertexBindings.size());
        vertexInput.pVertexBindingDescriptions      = vertexBindings.data();
        vertexInput.vertexAttributeDescriptionCount = static_cast<u32>(vertexAttrs.size());
        vertexInput.pVertexAttributeDescriptions    = vertexAttrs.data();

        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = ToVk(desc.topology);

        VkPipelineViewportStateCreateInfo viewport{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport.viewportCount = 1;
        viewport.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = VK_CULL_MODE_NONE;   // 2D geometry is single-sided
        raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable  = desc.depthTest  ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

        const VkPipelineColorBlendAttachmentState blend = BlendState(desc.blend);
        VkPipelineColorBlendStateCreateInfo blendState{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blendState.attachmentCount = 1;
        blendState.pAttachments    = &blend;

        const VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates    = dynamics;

        // Dynamic rendering: no VkRenderPass, no VkFramebuffer. The attachment formats declared
        // here must match the target this is used against, or validation fires at draw time
        // rather than here, where the mismatch would be obvious.
        const VkFormat colorFormat = ToVk(desc.colorFormat);
        VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount    = 1;
        rendering.pColorAttachmentFormats = &colorFormat;
        rendering.depthAttachmentFormat   = ToVk(desc.depthFormat);

        VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blendState;
        info.pDynamicState = &dynamic;
        info.layout = m_Layout;
        HEARTH_VK_CHECK(vkCreateGraphicsPipelines(m_Device.Raw(), m_Device.PipelineCache(), 1,
                                                  &info, nullptr, &m_Pipeline));
        SetObjectName(m_Device.Raw(), m_Pipeline, desc.debugName);
    }

    VulkanPipeline::~VulkanPipeline() {
        if (m_Pipeline)  vkDestroyPipeline(m_Device.Raw(), m_Pipeline, nullptr);
        if (m_Layout)    vkDestroyPipelineLayout(m_Device.Raw(), m_Layout, nullptr);
        if (m_SetLayout) vkDestroyDescriptorSetLayout(m_Device.Raw(), m_SetLayout, nullptr);
    }

    // ---------------------------------------------------------------------------- BindGroup

    VulkanBindGroup::VulkanBindGroup(VulkanDevice& device, const Ref<Pipeline>& pipeline,
                                     const std::vector<BindGroupEntry>& entries)
        : m_Device(device), m_Pipeline(pipeline) {
        auto* vkPipeline = static_cast<VulkanPipeline*>(pipeline.get());
        HEARTH_ASSERT(vkPipeline->HasBindings(), "pipeline '{}' declares no bindings to bind to",
                      vkPipeline->DebugName());
        m_PipelineLayout = vkPipeline->Layout();

        m_Allocation = m_Device.AllocateDescriptorSet(vkPipeline->SetLayout());
        m_Set = m_Allocation.set;
        SetObjectName(m_Device.Raw(), m_Set, vkPipeline->DebugName() + ".bindgroup");

        Update(entries);
    }

    VulkanBindGroup::~VulkanBindGroup() {
        // Back to the pool it came from, which is not necessarily the one being filled now.
        m_Device.FreeDescriptorSet(m_Allocation);
    }

    void VulkanBindGroup::Update(const std::vector<BindGroupEntry>& entries) {
        const auto& slots = m_Pipeline->Bindings();

        // Both info arrays are sized up front and never grown afterwards. The descriptor writes
        // hold raw pointers into them, so a reallocation partway through would leave earlier
        // writes pointing at freed memory -- and vkUpdateDescriptorSets reads them all at the end.
        std::vector<VkWriteDescriptorSet> writes;
        std::vector<VkDescriptorBufferInfo> bufferInfos;
        std::vector<std::vector<VkDescriptorImageInfo>> imageInfos;
        writes.reserve(entries.size());
        bufferInfos.reserve(entries.size());
        imageInfos.reserve(entries.size());

        for (const auto& entry : entries) {
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet         = m_Set;
            write.dstBinding     = entry.binding;
            write.descriptorType = ToVk(entry.type);

            if (entry.type == BindingType::StorageTexture) {
                HEARTH_ASSERT(entry.textures.size() == 1,
                              "binding {} is a storage texture and takes exactly one image",
                              entry.binding);
                auto* vt = static_cast<VulkanTexture*>(entry.textures.front().get());
                HEARTH_ASSERT(vt->Desc().usage == TextureUsage::Storage,
                              "binding {} needs a texture created with TextureUsage::Storage",
                              entry.binding);
                auto& infos = imageInfos.emplace_back();
                // A storage image is accessed in GENERAL, and hearth keeps it there for its
                // whole life rather than transitioning per use -- one that is both written by
                // compute and sampled cannot be in two layouts at once.
                infos.push_back({ VK_NULL_HANDLE, vt->View(), VK_IMAGE_LAYOUT_GENERAL });
                write.descriptorCount = 1;
                write.pImageInfo      = infos.data();
            } else if (entry.type == BindingType::SampledTexture) {
                HEARTH_ASSERT(!entry.textures.empty(),
                              "binding {} is a sampled texture with nothing bound to it",
                              entry.binding);

                auto& infos = imageInfos.emplace_back();

                u32 declared = 1;
                for (const auto& slot : slots)
                    if (slot.binding == entry.binding) { declared = slot.count; break; }
                HEARTH_ASSERT(entry.textures.size() <= declared,
                              "binding {} was given {} textures but the pipeline declares room "
                              "for {}", entry.binding, entry.textures.size(), declared);

                infos.reserve(declared);
                for (const auto& texture : entry.textures) {
                    auto* vt = static_cast<VulkanTexture*>(texture.get());
                    // A storage image stays in GENERAL for its whole life, so sampling one
                    // has to advertise that layout rather than the usual read-only optimal.
                    infos.push_back({ vt->Sampler(), vt->View(),
                                      vt->Desc().usage == TextureUsage::Storage
                                          ? VK_IMAGE_LAYOUT_GENERAL
                                          : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
                }
                // Pad the rest with the first entry. Sampling a descriptor that was never written
                // is undefined behaviour even in a branch the shader does not take, and a batcher
                // that has registered six of its thirty-two slots hits exactly that -- as a
                // device loss on one vendor and a black quad on another.
                while (infos.size() < declared) infos.push_back(infos.front());

                write.descriptorCount = static_cast<u32>(infos.size());
                write.pImageInfo      = infos.data();
            } else {
                HEARTH_ASSERT(entry.buffer, "binding {} is a buffer binding with no buffer",
                              entry.binding);
                auto* vb = static_cast<VulkanBuffer*>(entry.buffer.get());
                bufferInfos.push_back({ vb->Raw(), entry.bufferOffset,
                                        entry.bufferRange ? entry.bufferRange : VK_WHOLE_SIZE });
                write.descriptorCount = 1;
                write.pBufferInfo     = &bufferInfos.back();
            }
            writes.push_back(write);
        }

        vkUpdateDescriptorSets(m_Device.Raw(), static_cast<u32>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    // ---------------------------------------------------------------------------- RenderTarget

    VulkanRenderTarget::VulkanRenderTarget(VulkanDevice& device, const RenderTargetDesc& desc)
        : m_Device(device), m_Desc(desc) { Build(); }

    VulkanRenderTarget::~VulkanRenderTarget() { Destroy(); }

    void VulkanRenderTarget::Build() {
        TextureDesc color{};
        color.width     = m_Desc.width;
        color.height    = m_Desc.height;
        color.format    = m_Desc.colorFormat;
        color.usage     = TextureUsage::RenderTarget;
        color.debugName = m_Desc.debugName;
        m_Color = CreateRef<VulkanTexture>(m_Device, color);

        if (m_Desc.depthFormat == Format::Undefined) return;

        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType   = VK_IMAGE_TYPE_2D;
        info.format      = ToVk(m_Desc.depthFormat);
        info.extent      = { m_Desc.width, m_Desc.height, 1 };
        info.mipLevels   = 1;
        info.arrayLayers = 1;
        info.samples     = VK_SAMPLE_COUNT_1_BIT;
        info.tiling      = VK_IMAGE_TILING_OPTIMAL;
        info.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        HEARTH_VK_CHECK(vmaCreateImage(m_Device.Allocator(), &info, &alloc,
                                       &m_DepthImage, &m_DepthAllocation, nullptr));

        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image    = m_DepthImage;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format   = info.format;
        view.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        HEARTH_VK_CHECK(vkCreateImageView(m_Device.Raw(), &view, nullptr, &m_DepthView));
    }

    void VulkanRenderTarget::Destroy() {
        m_Color.reset();
        if (m_DepthView)  vkDestroyImageView(m_Device.Raw(), m_DepthView, nullptr);
        if (m_DepthImage) vmaDestroyImage(m_Device.Allocator(), m_DepthImage, m_DepthAllocation);
        m_DepthView  = VK_NULL_HANDLE;
        m_DepthImage = VK_NULL_HANDLE;
    }

    void VulkanRenderTarget::Resize(u32 width, u32 height) {
        if (width == m_Desc.width && height == m_Desc.height) return;
        if (width == 0 || height == 0) return;
        m_Device.WaitIdle();
        Destroy();
        m_Desc.width  = width;
        m_Desc.height = height;
        Build();
    }

    VkImageView VulkanRenderTarget::ColorView() const { return m_Color->View(); }

    void VulkanRenderTarget::ReadPixels(void* outPixels, u64 size) {
        const u64 expected = u64(m_Desc.width) * m_Desc.height * FormatSize(m_Desc.colorFormat);
        HEARTH_ASSERT(size >= expected,
                      "readback buffer of {} bytes is short of the {} bytes {}x{} {} needs",
                      size, expected, m_Desc.width, m_Desc.height, FormatName(m_Desc.colorFormat));

        VulkanBuffer readback(m_Device, BufferDesc{ expected, BufferUsage::Staging,
                                                    MemoryKind::HostVisible, "readback" });

        const VkImageLayout from = m_Color->Layout();
        m_Device.ImmediateSubmitRaw([&](VkCommandBuffer cmd) {
            TransitionImageRaw(cmd, m_Color->Image(), from,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { m_Desc.width, m_Desc.height, 1 };
            vkCmdCopyImageToBuffer(cmd, m_Color->Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback.Raw(), 1, &copy);

            TransitionImageRaw(cmd, m_Color->Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_IMAGE_ASPECT_COLOR_BIT);
        });
        m_Color->SetLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        std::memcpy(outPixels, readback.Map(), expected);
    }

}
