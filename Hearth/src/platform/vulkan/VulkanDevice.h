#pragma once

#include "hearth/Device.h"
#include "platform/vulkan/VulkanCommon.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hearth {

    class VulkanSwapchain;
    class VulkanCommandList;

    class VulkanDevice final : public Device {
    public:
        explicit VulkanDevice(const DeviceDesc& desc);
        ~VulkanDevice() override;

        bool Ok() const { return m_Ok; }

        const DeviceCaps& Caps() const override { return m_Caps; }
        Swapchain* GetSwapchain() override;

        Ref<Buffer>       CreateBuffer(const BufferDesc&) override;
        Ref<Texture>      CreateTexture(const TextureDesc&) override;
        Ref<Shader>       CreateShader(const ShaderDesc&) override;
        Ref<Pipeline>     CreatePipeline(const PipelineDesc&) override;
        Ref<Pipeline>     CreateComputePipeline(const ComputePipelineDesc&) override;
        Ref<BindGroup>    CreateBindGroup(const Ref<Pipeline>&,
                                          const std::vector<BindGroupEntry>&) override;
        Ref<RenderTarget> CreateRenderTarget(const RenderTargetDesc&) override;

        CommandList* BeginFrame() override;
        void EndFrame() override;
        void WaitIdle() override;
        void SubmitImmediate(const std::function<void(CommandList&)>& record) override;

        void OnWindowResize(u32 width, u32 height) override;
        void OnSurfaceLost() override;
        void OnSurfaceRecreated(Surface& surface) override;

        bool DeviceLost() const override { return m_DeviceLost; }
        void ReportDeviceLost(const char* where);

        // --- used by the other Vulkan objects and by hearth/vulkan/Native.h -------------------
        VkDevice         Raw() const { return m_Device; }
        VkPhysicalDevice Physical() const { return m_Physical; }
        VkInstance       Instance() const { return m_Instance; }
        VmaAllocator     Allocator() const { return m_Allocator; }
        VkQueue          GraphicsQueue() const { return m_GraphicsQueue; }
        u32              GraphicsQueueFamily() const { return m_GraphicsFamily; }
        // A descriptor set together with the pool it came from, because the pool that can free
        // it is not necessarily the one currently being filled.
        struct DescriptorAllocation {
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkDescriptorPool pool = VK_NULL_HANDLE;
        };
        DescriptorAllocation AllocateDescriptorSet(VkDescriptorSetLayout layout);
        void FreeDescriptorSet(const DescriptorAllocation& allocation);

        // The pool currently being filled. For hearth/vulkan/Native.h; a caller that allocates
        // from it directly should expect it to be exhausted eventually and handle that itself.
        VkDescriptorPool DescriptorPool() const { return m_DescriptorPools.back(); }
        VkPipelineCache  PipelineCache() const { return m_PipelineCache; }

        void ImmediateSubmitRaw(const std::function<void(VkCommandBuffer)>& record);

        VkCommandBuffer CurrentCommandBuffer() const;
        VulkanSwapchain* SwapchainImpl() const { return m_Swapchain.get(); }

        // Samplers are deduplicated by their settings. A texture does not own one: a batcher with
        // four hundred glyph pages that all sample the same way would otherwise burn four hundred
        // sampler objects against maxSamplerAllocationCount, which on some drivers is 4000 and is
        // a limit nobody thinks about until a font atlas trips it.
        VkSampler SamplerFor(Filter minFilter, Filter magFilter, AddressMode address);

    private:
        bool InitVulkan(const DeviceDesc&);
        bool InitAllocator();
        void InitPipelineCache(const std::string& path);
        void SavePipelineCache();
        VkDescriptorPool AddDescriptorPool();
        bool InitFrames();
        void DestroyFrames();
        bool BuildSwapchain();
        void FramebufferSize(u32& width, u32& height) const;

        struct Frame {
            VkCommandPool   pool = VK_NULL_HANDLE;
            VkCommandBuffer cmd  = VK_NULL_HANDLE;
            VkSemaphore     imageAvailable = VK_NULL_HANDLE;
            VkFence         inFlight = VK_NULL_HANDLE;
        };

        Surface* m_Surface = nullptr;

        VkInstance               m_Instance  = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_Messenger = VK_NULL_HANDLE;
        VkSurfaceKHR             m_VkSurface = VK_NULL_HANDLE;
        VkPhysicalDevice         m_Physical  = VK_NULL_HANDLE;
        VkDevice                 m_Device    = VK_NULL_HANDLE;
        VkQueue                  m_GraphicsQueue = VK_NULL_HANDLE;
        u32                      m_GraphicsFamily = 0;
        VmaAllocator             m_Allocator = nullptr;
        // A chain, not a single pool. A renderer that keeps discovering textures keeps
        // allocating sets, and a pool sized once at startup is a ceiling that shows up as a
        // failed allocation on somebody else's machine. When one fills, the next is twice as big.
        std::vector<VkDescriptorPool> m_DescriptorPools;
        u32 m_DescriptorPoolCapacity = 0;

        VkCommandPool m_ImmediatePool  = VK_NULL_HANDLE;
        VkFence       m_ImmediateFence = VK_NULL_HANDLE;
        // One pool and one fence, so two threads staging an upload at the same time would
        // otherwise hand each other's command buffers to the queue.
        std::mutex    m_ImmediateMutex;

        VkPipelineCache m_PipelineCache = VK_NULL_HANDLE;
        std::string     m_PipelineCachePath;

        Scope<VulkanSwapchain>   m_Swapchain;
        Scope<VulkanCommandList> m_CommandList;
        Scope<VulkanCommandList> m_ImmediateList;

        std::unordered_map<u32, VkSampler> m_Samplers;
        std::mutex m_SamplerMutex;
        // Guards the pool chain: an allocation can append a pool, which reallocates the vector
        // another thread may be reading.
        std::mutex m_DescriptorMutex;

        std::vector<Frame> m_Frames;
        u32  m_FrameIndex = 0;
        u32  m_ImageIndex = 0;
        bool m_FrameOpen  = false;
        bool m_Ok         = false;
        bool m_Validation = false;
        bool m_VSync      = true;
        bool m_DeviceLost = false;
        // Set between OnSurfaceLost and OnSurfaceRecreated. Distinct from a zero-sized swapchain:
        // there is no VkSurfaceKHR at all, so even a resize has nothing to rebuild against.
        bool m_SurfaceGone = false;

        u32 m_ApiVersion = 0;
        DeviceCaps m_Caps;
    };

}
