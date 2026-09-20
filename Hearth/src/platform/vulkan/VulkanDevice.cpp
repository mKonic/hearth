#include "platform/vulkan/VulkanDevice.h"

#include "platform/vulkan/VulkanCommandList.h"
#include "platform/vulkan/VulkanResources.h"
#include "platform/vulkan/VulkanSwapchain.h"
#include "platform/vulkan/VulkanVersion.h"

#include <VkBootstrap.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace hearth {

    namespace {

        VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT,
            const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                HEARTH_ERROR("[validation] {}", data->pMessage);
            else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                HEARTH_WARN("[validation] {}", data->pMessage);
            return VK_FALSE;
        }

        u32 SamplerKey(Filter minFilter, Filter magFilter, AddressMode address,
                       u32 mipLevels, u32 anisotropy) {
            // mipLevels only matters as a maxLod, and anisotropy is clamped to a small
            // integer before it gets here, so both fit alongside the three enums.
            return static_cast<u32>(minFilter)
                 | (static_cast<u32>(magFilter) << 2)
                 | (static_cast<u32>(address)   << 4)
                 | ((mipLevels & 0x1Fu)         << 8)
                 | ((anisotropy & 0x1Fu)        << 16);
        }

    }

    VulkanDevice::VulkanDevice(const DeviceDesc& desc)
        : m_Surface(desc.surface), m_VSync(desc.vsync) {
        if (!InitVulkan(desc))   return;
        if (!InitAllocator())    return;
        if (!InitFrames())       return;
        InitPipelineCache(desc.pipelineCachePath);
        if (m_Surface && !BuildSwapchain()) return;

        m_CommandList   = CreateScope<VulkanCommandList>(*this);
        m_ImmediateList = CreateScope<VulkanCommandList>(*this);
        m_Ok = true;
    }

    VulkanDevice::~VulkanDevice() {
        if (m_Device) vkDeviceWaitIdle(m_Device);
        SavePipelineCache();
        if (m_PipelineCache) vkDestroyPipelineCache(m_Device, m_PipelineCache, nullptr);
        m_Swapchain.reset();
        DestroyFrames();
        for (auto& [key, sampler] : m_Samplers) vkDestroySampler(m_Device, sampler, nullptr);
        if (m_ImmediateFence) vkDestroyFence(m_Device, m_ImmediateFence, nullptr);
        if (m_ImmediatePool)  vkDestroyCommandPool(m_Device, m_ImmediatePool, nullptr);
        for (auto pool : m_DescriptorPools) vkDestroyDescriptorPool(m_Device, pool, nullptr);
        if (m_Allocator)  vmaDestroyAllocator(m_Allocator);
        if (m_Device)     vkDestroyDevice(m_Device, nullptr);
        if (m_VkSurface)  vkDestroySurfaceKHR(m_Instance, m_VkSurface, nullptr);
        if (m_Messenger)  vkb::destroy_debug_utils_messenger(m_Instance, m_Messenger);
        if (m_Instance)   vkDestroyInstance(m_Instance, nullptr);
    }

    // Instance, surface and device are one step: vk-bootstrap's PhysicalDeviceSelector needs the
    // live vkb::Instance -- its api version, its enabled extensions and its loader function
    // pointers -- and none of that can be rebuilt from a bare VkInstance handle afterwards.
    bool VulkanDevice::InitVulkan(const DeviceDesc& desc) {
        m_Validation = desc.enableValidation;

        // `request_validation_layers` is a request, not a requirement: when the layers are not
        // installed vk-bootstrap quietly builds the instance without them and succeeds. Asking
        // the system first is the only way to tell the difference, and the difference matters
        // -- "validation on" in the log when nothing is validating is worse than no message,
        // because it is the line someone reads before concluding their code is clean.
        if (m_Validation) {
            auto systemInfo = vkb::SystemInfo::get_system_info();
            if (!systemInfo || !systemInfo->validation_layers_available) {
                HEARTH_WARN("validation was requested but the layers are not installed "
                            "(Arch: vulkan-validationlayers); continuing without them");
                m_Validation = false;
            }
        }

        const u32 instanceVersion = NegotiateInstanceVersion();
        if (instanceVersion == 0) return false;   // already reported, with the reason

        const u32 floor = std::max(desc.minimumApiVersion, kMinimumApiVersion);
        if (instanceVersion < floor) {
            HEARTH_ERROR("this application asked for Vulkan {}.{} but the loader offers {}.{}",
                         VK_API_VERSION_MAJOR(floor), VK_API_VERSION_MINOR(floor),
                         VK_API_VERSION_MAJOR(instanceVersion),
                         VK_API_VERSION_MINOR(instanceVersion));
            return false;
        }

        const std::vector<const char*> extra =
            m_Surface ? m_Surface->RequiredInstanceExtensions() : std::vector<const char*>{};

        auto build = [&](bool validation) {
            vkb::InstanceBuilder builder;
            builder.set_app_name(desc.appName.c_str())
                   .set_engine_name(desc.engineName.c_str())
                   .require_api_version(instanceVersion);
            for (const char* ext : extra) builder.enable_extension(ext);
            if (validation)
                builder.request_validation_layers(true).set_debug_callback(DebugCallback);
            return builder.build();
        };

        auto result = build(m_Validation);
        if (!result && m_Validation) {
            // A machine with no validation layers installed is a normal machine, not a broken one,
            // and it should still run. Say so once and carry on without them.
            HEARTH_WARN("instance creation with validation failed ({}); retrying without",
                        result.error().message());
            m_Validation = false;
            result = build(false);
        }
        if (!result) {
            HEARTH_ERROR("vulkan instance creation failed: {}", result.error().message());
            return false;
        }

        vkb::Instance instance = result.value();
        m_Instance  = instance.instance;
        m_Messenger = instance.debug_messenger;

        if (m_Surface) {
            const VkResult created = m_Surface->CreateVulkanSurface(m_Instance, &m_VkSurface);
            if (created != VK_SUCCESS) {
                HEARTH_ERROR("the host could not create a presentation surface: {}",
                             VkResultName(created));
                return false;
            }
        }

        // 1.3 dynamic rendering + synchronization2: there is no VkRenderPass and no VkFramebuffer
        // object anywhere in this library.
        VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        f13.dynamicRendering = VK_TRUE;
        f13.synchronization2 = VK_TRUE;
        // Every one of these is a feature a Vulkan 1.3 device is REQUIRED by the spec to
        // support, so asking for them narrows nothing. They are enabled because a shader that
        // declares the matching SPIR-V capability -- which glslc emits for `discard` on its
        // own -- fails vkCreateShaderModule if the feature was not switched on, and the error
        // names the capability rather than the line of GLSL that caused it.
        f13.shaderDemoteToHelperInvocation     = VK_TRUE;
        f13.shaderTerminateInvocation          = VK_TRUE;
        f13.shaderZeroInitializeWorkgroupMemory = VK_TRUE;
        f13.subgroupSizeControl                = VK_TRUE;
        f13.computeFullSubgroups               = VK_TRUE;
        f13.maintenance4                       = VK_TRUE;

        // Descriptor indexing is what lets one draw address many textures -- a sprite batch, a
        // glyph atlas spread over several pages. partiallyBound is what makes an array binding
        // legal to leave half-written.
        VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        f12.descriptorIndexing = VK_TRUE;
        f12.runtimeDescriptorArray = VK_TRUE;
        f12.descriptorBindingPartiallyBound = VK_TRUE;
        f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        // What makes VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT legal on a sampler array,
        // and what makes rewriting one that is already bound legal. Only the sampled-image
        // variants are required: the uniform-buffer one is genuinely absent on some hardware,
        // so VulkanPipeline applies update-after-bind to texture arrays only.
        f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;


        vkb::PhysicalDeviceSelector selector{ instance };
        selector.set_minimum_version(floor)
                .set_required_features_13(f13)
                .set_required_features_12(f12);

        if (m_VkSurface) selector.set_surface(m_VkSurface);
        else             selector.defer_surface_initialization();

        auto physical = selector.select();
        if (!physical) {
            HEARTH_ERROR("no Vulkan 1.3 device with dynamic rendering and descriptor indexing: {}",
                         physical.error().message());
            return false;
        }

        vkb::PhysicalDevice chosen = physical.value();

        // Anisotropic filtering is optional in the spec, so it is enabled if the device has
        // it rather than required: a device without it still runs, and Caps().maxAnisotropy
        // reports 1 so a caller can tell.
        VkPhysicalDeviceFeatures optionalCore{};
        optionalCore.samplerAnisotropy = VK_TRUE;
        const bool anisotropy = chosen.enable_features_if_present(optionalCore);

        VkPhysicalDeviceProperties selected{};
        vkGetPhysicalDeviceProperties(chosen.physical_device, &selected);
        // The usable version is the lowest of the three parties: loader, headers, device.
        m_ApiVersion = std::min(instanceVersion, selected.apiVersion);

        OptionalFeatureStorage optionalStorage;
        std::vector<void*> optionalChain;
        const OptionalFeatures optional = ChainOptionalFeatures(
            chosen.physical_device, m_ApiVersion, optionalStorage, optionalChain);

        vkb::DeviceBuilder deviceBuilder{ chosen };
        for (void* link : optionalChain) deviceBuilder.add_pNext(link);

        auto built = deviceBuilder.build();
        if (!built) {
            HEARTH_ERROR("logical device creation failed: {}", built.error().message());
            return false;
        }

        m_Physical       = chosen.physical_device;
        m_Device         = built.value().device;
        m_GraphicsQueue  = built.value().get_queue(vkb::QueueType::graphics).value();
        m_GraphicsFamily = built.value().get_queue_index(vkb::QueueType::graphics).value();

        // The plain maxPerStageDescriptorSampledImages is the wrong limit to report: an array
        // binding a batcher rewrites while it is bound needs UPDATE_AFTER_BIND, and that has its
        // own, usually much larger, cap.
        VkPhysicalDeviceDescriptorIndexingProperties indexing{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES };
        VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props2.pNext = &indexing;
        vkGetPhysicalDeviceProperties2(m_Physical, &props2);
        const VkPhysicalDeviceProperties& props = props2.properties;
        m_Caps.deviceName = props.deviceName;
        m_Caps.discrete   = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        m_Caps.softwareRasterizer = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        m_Caps.maxTextureSize = props.limits.maxImageDimension2D;
        m_Caps.maxAnisotropy = anisotropy ? props.limits.maxSamplerAnisotropy : 1.0f;
        m_Caps.maxColorAttachments = props.limits.maxColorAttachments;
        // The highest count BOTH colour and depth support. A target multisamples them
        // together, so the useful figure is the intersection rather than either alone.
        const VkSampleCountFlags sampleMask = props.limits.framebufferColorSampleCounts
                                            & props.limits.framebufferDepthSampleCounts;
        for (u32 count : { 64u, 32u, 16u, 8u, 4u, 2u }) {
            if (sampleMask & count) { m_Caps.maxSamples = count; break; }
        }
        m_Caps.maxTexturesPerBindGroup =
            std::max(indexing.maxDescriptorSetUpdateAfterBindSampledImages,
                     props.limits.maxPerStageDescriptorSampledImages);
        m_Caps.apiVersion       = m_ApiVersion;
        m_Caps.validationActive = m_Validation;
        m_Caps.hostImageCopy  = optional.hostImageCopy;
        m_Caps.pushDescriptor = optional.pushDescriptor;
        m_Caps.maintenance5   = optional.maintenance5;
        m_Caps.driverInfo     = "Vulkan " + DescribeOptional(m_ApiVersion, optional);

        HEARTH_INFO("{} ({}){}, {}", m_Caps.deviceName,
                    m_Caps.discrete ? "discrete" : (m_Caps.softwareRasterizer ? "CPU" : "integrated"),
                    m_Validation ? ", validation on" : "", m_Caps.driverInfo);
        return true;
    }

    bool VulkanDevice::InitAllocator() {
        // VMA is compiled with dynamic function loading, so it needs the two proc-address entry
        // points handed to it explicitly.
        VmaVulkanFunctions fns{};
        fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        fns.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

        VmaAllocatorCreateInfo info{};
        info.physicalDevice   = m_Physical;
        info.device           = m_Device;
        info.instance         = m_Instance;
        info.vulkanApiVersion = m_ApiVersion;
        info.pVulkanFunctions = &fns;
        HEARTH_VK_CHECK(vmaCreateAllocator(&info, &m_Allocator));

        AddDescriptorPool();
        return true;
    }

    // Each pool in the chain is twice the size of the one before, starting at a size that
    // comfortably holds one batcher's texture array. Nothing here is a ceiling: when a pool
    // refuses an allocation, the next one is created and the allocation is retried.
    VkDescriptorPool VulkanDevice::AddDescriptorPool() {
        m_DescriptorPoolCapacity = m_DescriptorPoolCapacity ? m_DescriptorPoolCapacity * 2 : 64;
        const u32 sets = m_DescriptorPoolCapacity;
        // Big enough for either many small sets or one large array binding, whichever this pool
        // is asked for first. A batcher's array is a single set that can want thousands of
        // descriptors on its own, and sizing only per-set would refuse it outright.
        const u32 sampledImages = std::max(sets * 16u,
                                           std::min(m_Caps.maxTexturesPerBindGroup, 4096u));

        const VkDescriptorPoolSize sizes[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         sets * 4 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         sets * 4 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampledImages },
        };
        VkDescriptorPoolCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        // UPDATE_AFTER_BIND so a renderer can rewrite an array binding that is already bound --
        // a sprite batcher that discovers a texture mid-scene does exactly that.
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT
                   | VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        info.maxSets       = sets;
        info.poolSizeCount = static_cast<u32>(std::size(sizes));
        info.pPoolSizes    = sizes;

        VkDescriptorPool pool = VK_NULL_HANDLE;
        HEARTH_VK_CHECK(vkCreateDescriptorPool(m_Device, &info, nullptr, &pool));
        m_DescriptorPools.push_back(pool);
        return pool;
    }

    VulkanDevice::DescriptorAllocation VulkanDevice::AllocateDescriptorSet(VkDescriptorSetLayout layout) {
        const std::scoped_lock lock(m_DescriptorMutex);

        VkDescriptorSetAllocateInfo alloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc.descriptorPool     = m_DescriptorPools.back();
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts        = &layout;

        DescriptorAllocation result{};
        VkResult status = vkAllocateDescriptorSets(m_Device, &alloc, &result.set);
        if (status == VK_ERROR_OUT_OF_POOL_MEMORY || status == VK_ERROR_FRAGMENTED_POOL) {
            alloc.descriptorPool = AddDescriptorPool();
            status = vkAllocateDescriptorSets(m_Device, &alloc, &result.set);
        }
        HEARTH_ASSERT(status == VK_SUCCESS, "descriptor set allocation failed: {}",
                      VkResultName(status));
        result.pool = alloc.descriptorPool;
        return result;
    }

    void VulkanDevice::FreeDescriptorSet(const DescriptorAllocation& allocation) {
        const std::scoped_lock lock(m_DescriptorMutex);
        if (allocation.set)
            vkFreeDescriptorSets(m_Device, allocation.pool, 1, &allocation.set);
    }

    // A pipeline cache is a driver-owned blob: it turns the second and later compilations of
    // the same pipeline into a lookup. Persisting it across runs is what removes the cold
    // start; within one run it still pays off for pipelines that differ only by blend mode.
    void VulkanDevice::InitPipelineCache(const std::string& path) {
        m_PipelineCachePath = path;

        std::vector<char> blob;
        if (!path.empty()) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (file) {
                const auto size = file.tellg();
                if (size > 0) {
                    blob.resize(static_cast<size_t>(size));
                    file.seekg(0);
                    file.read(blob.data(), size);
                }
            }
        }

        VkPipelineCacheCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        info.initialDataSize = blob.size();
        info.pInitialData    = blob.empty() ? nullptr : blob.data();

        // A blob from another GPU, another driver version or a truncated write is rejected by
        // the driver on its own header check, so a stale file costs a cold start and nothing
        // worse. It is not worth validating here, and it is not worth failing over.
        if (vkCreatePipelineCache(m_Device, &info, nullptr, &m_PipelineCache) != VK_SUCCESS) {
            HEARTH_WARN("pipeline cache could not be created; pipelines will compile cold");
            m_PipelineCache = VK_NULL_HANDLE;
        }
    }

    void VulkanDevice::SavePipelineCache() {
        if (!m_PipelineCache || m_PipelineCachePath.empty()) return;

        size_t size = 0;
        if (vkGetPipelineCacheData(m_Device, m_PipelineCache, &size, nullptr) != VK_SUCCESS || !size)
            return;

        std::vector<char> blob(size);
        if (vkGetPipelineCacheData(m_Device, m_PipelineCache, &size, blob.data()) != VK_SUCCESS)
            return;

        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(m_PipelineCachePath).parent_path(), ec);

        // Write beside the target and rename, so a process killed mid-write leaves the
        // previous cache intact rather than a half file the next run has to reject.
        const std::string temp = m_PipelineCachePath + ".tmp";
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            if (!file) return;
            file.write(blob.data(), static_cast<std::streamsize>(blob.size()));
            if (!file) return;
        }
        std::filesystem::rename(temp, m_PipelineCachePath, ec);
    }

    bool VulkanDevice::InitFrames() {
        m_Frames.resize(m_Caps.framesInFlight);
        for (auto& frame : m_Frames) {
            VkCommandPoolCreateInfo pool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = m_GraphicsFamily;
            HEARTH_VK_CHECK(vkCreateCommandPool(m_Device, &pool, nullptr, &frame.pool));

            VkCommandBufferAllocateInfo alloc{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            alloc.commandPool = frame.pool;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = 1;
            HEARTH_VK_CHECK(vkAllocateCommandBuffers(m_Device, &alloc, &frame.cmd));

            VkSemaphoreCreateInfo sem{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            HEARTH_VK_CHECK(vkCreateSemaphore(m_Device, &sem, nullptr, &frame.imageAvailable));

            VkFenceCreateInfo fence{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // the first frame must not block forever
            HEARTH_VK_CHECK(vkCreateFence(m_Device, &fence, nullptr, &frame.inFlight));
        }

        VkCommandPoolCreateInfo pool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = m_GraphicsFamily;
        HEARTH_VK_CHECK(vkCreateCommandPool(m_Device, &pool, nullptr, &m_ImmediatePool));

        VkFenceCreateInfo fence{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        HEARTH_VK_CHECK(vkCreateFence(m_Device, &fence, nullptr, &m_ImmediateFence));
        return true;
    }

    void VulkanDevice::DestroyFrames() {
        for (auto& frame : m_Frames) {
            if (frame.inFlight)       vkDestroyFence(m_Device, frame.inFlight, nullptr);
            if (frame.imageAvailable) vkDestroySemaphore(m_Device, frame.imageAvailable, nullptr);
            if (frame.pool)           vkDestroyCommandPool(m_Device, frame.pool, nullptr);
        }
        m_Frames.clear();
    }

    void VulkanDevice::FramebufferSize(u32& width, u32& height) const {
        width = height = 0;
        if (m_Surface) m_Surface->FramebufferSize(width, height);
    }

    bool VulkanDevice::BuildSwapchain() {
        u32 width = 0, height = 0;
        FramebufferSize(width, height);
        m_Swapchain = CreateScope<VulkanSwapchain>(*this, m_VkSurface, width, height, m_VSync);
        // A zero-sized surface is a minimized window, not a failure: the chain comes back when
        // the window does, and BeginFrame skips frames until then.
        if (!m_Swapchain->Valid() && width != 0 && height != 0) {
            HEARTH_ERROR("swapchain creation failed");
            return false;
        }
        return true;
    }

    Swapchain* VulkanDevice::GetSwapchain() { return m_Swapchain.get(); }

    VkSampler VulkanDevice::SamplerFor(Filter minFilter, Filter magFilter, AddressMode address,
                                       u32 mipLevels, f32 maxAnisotropy) {
        const std::scoped_lock lock(m_SamplerMutex);

        // Asking for more anisotropy than the device offers is a request, not an error: clamp
        // it and carry on, because refusing to create the texture would be a worse answer to
        // "this machine's filtering is not as good as that one's".
        const f32 anisotropy = m_Caps.maxAnisotropy > 1.0f
                             ? std::min(maxAnisotropy, m_Caps.maxAnisotropy)
                             : 1.0f;

        const u32 key = SamplerKey(minFilter, magFilter, address, mipLevels,
                                   static_cast<u32>(anisotropy));
        if (auto it = m_Samplers.find(key); it != m_Samplers.end()) return it->second;

        VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        info.minFilter = ToVk(minFilter);
        info.magFilter = ToVk(magFilter);
        info.addressModeU = info.addressModeV = info.addressModeW = ToVk(address);
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        // Clamped to the levels this texture actually has. VK_LOD_CLAMP_NONE on a
        // single-level image samples a mip that is not there.
        info.maxLod = static_cast<f32>(mipLevels);
        if (anisotropy > 1.0f) {
            info.anisotropyEnable = VK_TRUE;
            info.maxAnisotropy    = anisotropy;
        }

        VkSampler sampler = VK_NULL_HANDLE;
        HEARTH_VK_CHECK(vkCreateSampler(m_Device, &info, nullptr, &sampler));
        // Shared between every texture with these settings, so it is named for the settings
        // rather than for whichever texture happened to ask for it first.
        SetObjectName(m_Device, sampler,
                      std::format("sampler({}/{}/{}/mips{}/aniso{})",
                                  minFilter == Filter::Nearest ? "nearest" : "linear",
                                  magFilter == Filter::Nearest ? "nearest" : "linear",
                                  static_cast<int>(address), mipLevels,
                                  static_cast<int>(anisotropy)));
        m_Samplers.emplace(key, sampler);
        return sampler;
    }

    Ref<Buffer>  VulkanDevice::CreateBuffer(const BufferDesc& d)  { return CreateRef<VulkanBuffer>(*this, d); }
    Ref<Texture> VulkanDevice::CreateTexture(const TextureDesc& d){ return CreateRef<VulkanTexture>(*this, d); }
    Ref<Shader>  VulkanDevice::CreateShader(const ShaderDesc& d)  { return CreateRef<VulkanShader>(*this, d); }
    Ref<Pipeline> VulkanDevice::CreatePipeline(const PipelineDesc& d) { return CreateRef<VulkanPipeline>(*this, d); }
    Ref<Pipeline> VulkanDevice::CreateComputePipeline(const ComputePipelineDesc& d) { return CreateRef<VulkanPipeline>(*this, d); }

    Ref<BindGroup> VulkanDevice::CreateBindGroup(const Ref<Pipeline>& p,
                                                 const std::vector<BindGroupEntry>& e) {
        return CreateRef<VulkanBindGroup>(*this, p, e);
    }

    Ref<RenderTarget> VulkanDevice::CreateRenderTarget(const RenderTargetDesc& d) {
        return CreateRef<VulkanRenderTarget>(*this, d);
    }

    VkCommandBuffer VulkanDevice::CurrentCommandBuffer() const {
        return m_FrameOpen ? m_Frames[m_FrameIndex].cmd : VK_NULL_HANDLE;
    }

    void VulkanDevice::ImmediateSubmitRaw(const std::function<void(VkCommandBuffer)>& record) {
        // Serialised: the pool, the fence and the queue submission are all shared, and an
        // asset loader uploading textures from several threads is the expected caller.
        const std::scoped_lock lock(m_ImmediateMutex);

        VkCommandBufferAllocateInfo alloc{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        alloc.commandPool = m_ImmediatePool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        HEARTH_VK_CHECK(vkAllocateCommandBuffers(m_Device, &alloc, &cmd));

        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        HEARTH_VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
        record(cmd);
        HEARTH_VK_CHECK(vkEndCommandBuffer(cmd));

        VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdInfo.commandBuffer = cmd;
        VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;

        HEARTH_VK_CHECK(vkQueueSubmit2(m_GraphicsQueue, 1, &submit, m_ImmediateFence));
        HEARTH_VK_CHECK(vkWaitForFences(m_Device, 1, &m_ImmediateFence, VK_TRUE, UINT64_MAX));
        HEARTH_VK_CHECK(vkResetFences(m_Device, 1, &m_ImmediateFence));
        vkFreeCommandBuffers(m_Device, m_ImmediatePool, 1, &cmd);
    }

    void VulkanDevice::SubmitImmediate(const std::function<void(CommandList&)>& record) {
        HEARTH_ASSERT(!m_FrameOpen, "SubmitImmediate inside an open frame: it blocks on the GPU, "
                                    "which would stall the frame being recorded");
        ImmediateSubmitRaw([&](VkCommandBuffer cmd) {
            m_ImmediateList->Begin(cmd, 0, /*offscreenOnly*/ true);
            record(*m_ImmediateList);
            m_ImmediateList->End();
        });
    }

    CommandList* VulkanDevice::BeginFrame() {
        if (!m_Ok || m_DeviceLost || m_SurfaceGone) return nullptr;
        if (m_Swapchain && !m_Swapchain->Valid()) return nullptr;   // minimized

        Frame& frame = m_Frames[m_FrameIndex];
        HEARTH_VK_CHECK(vkWaitForFences(m_Device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

        if (m_Swapchain) {
            const VkResult acquired = vkAcquireNextImageKHR(m_Device, m_Swapchain->Raw(), UINT64_MAX,
                                                            frame.imageAvailable, VK_NULL_HANDLE,
                                                            &m_ImageIndex);
            if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
                u32 width = 0, height = 0;
                FramebufferSize(width, height);
                m_Swapchain->Resize(width, height);
                return nullptr;                 // skip this frame; the next uses the new chain
            }
            if (acquired == VK_ERROR_DEVICE_LOST) { ReportDeviceLost("vkAcquireNextImageKHR"); return nullptr; }
            if (acquired == VK_ERROR_SURFACE_LOST_KHR) {
                HEARTH_WARN("the presentation surface was lost outside the host's lifecycle "
                            "callbacks; call OnSurfaceLost/OnSurfaceRecreated to rebuild it");
                m_SurfaceGone = true;
                return nullptr;
            }
            if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
                HEARTH_ERROR("vkAcquireNextImageKHR: {}", VkResultName(acquired));
                return nullptr;
            }
        }

        HEARTH_VK_CHECK(vkResetFences(m_Device, 1, &frame.inFlight));
        HEARTH_VK_CHECK(vkResetCommandBuffer(frame.cmd, 0));

        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        HEARTH_VK_CHECK(vkBeginCommandBuffer(frame.cmd, &begin));

        m_CommandList->Begin(frame.cmd, m_ImageIndex, /*offscreenOnly*/ false);
        m_FrameOpen = true;
        return m_CommandList.get();
    }

    void VulkanDevice::EndFrame() {
        if (!m_FrameOpen) return;
        m_FrameOpen = false;

        Frame& frame = m_Frames[m_FrameIndex];

        // A frame that drew nothing into the swapchain still has to leave the image presentable,
        // or the present validates as a layout mismatch.
        if (m_Swapchain) {
            const VkImageLayout from = m_CommandList->TouchedSwapchain()
                                     ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                     : VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            b.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstStageMask  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
            b.oldLayout = from;
            b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = m_Swapchain->Image(m_ImageIndex);
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(frame.cmd, &dep);
        }

        HEARTH_VK_CHECK(vkEndCommandBuffer(frame.cmd));
        m_CommandList->End();

        VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdInfo.commandBuffer = frame.cmd;

        VkSemaphoreSubmitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        wait.semaphore = frame.imageAvailable;
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        if (m_Swapchain) signal.semaphore = m_Swapchain->RenderFinished(m_ImageIndex);
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

        VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        if (m_Swapchain) {
            submit.waitSemaphoreInfoCount = 1;
            submit.pWaitSemaphoreInfos = &wait;
            submit.signalSemaphoreInfoCount = 1;
            submit.pSignalSemaphoreInfos = &signal;
        }
        HEARTH_VK_CHECK(vkQueueSubmit2(m_GraphicsQueue, 1, &submit, frame.inFlight));

        if (m_Swapchain) {
            VkSwapchainKHR chain = m_Swapchain->Raw();
            VkSemaphore renderFinished = m_Swapchain->RenderFinished(m_ImageIndex);
            VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &renderFinished;
            present.swapchainCount = 1;
            present.pSwapchains = &chain;
            present.pImageIndices = &m_ImageIndex;

            const VkResult presented = vkQueuePresentKHR(m_GraphicsQueue, &present);
            if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
                u32 width = 0, height = 0;
                FramebufferSize(width, height);
                m_Swapchain->Resize(width, height);
            } else if (presented == VK_ERROR_DEVICE_LOST) {
                ReportDeviceLost("vkQueuePresentKHR");
            } else if (presented == VK_ERROR_SURFACE_LOST_KHR) {
                m_SurfaceGone = true;
            } else if (presented != VK_SUCCESS) {
                HEARTH_ERROR("vkQueuePresentKHR: {}", VkResultName(presented));
            }
        }

        m_FrameIndex = (m_FrameIndex + 1) % static_cast<u32>(m_Frames.size());
    }

    // A driver reset, a GPU hang, a suspend that did not survive: every handle this device ever
    // handed out is now invalid and every call from here on returns the same error. Say it once,
    // in terms someone can act on, and let the host decide -- a window that keeps spinning on a
    // dead device looks like a hang, and an assert mid-frame says nothing about why.
    void VulkanDevice::ReportDeviceLost(const char* where) {
        if (m_DeviceLost) return;
        m_DeviceLost = true;
        HEARTH_ERROR("the graphics device was lost ({}). This is a driver reset, a GPU hang or a "
                     "suspend the driver did not survive; nothing here can draw again until the "
                     "process restarts.", where);
    }

    void VulkanDevice::WaitIdle() { if (m_Device) vkDeviceWaitIdle(m_Device); }

    void VulkanDevice::OnWindowResize(u32 width, u32 height) {
        if (m_Swapchain && !m_SurfaceGone) m_Swapchain->Resize(width, height);
    }

    void VulkanDevice::OnSurfaceLost() {
        if (m_SurfaceGone) return;
        if (m_Device) vkDeviceWaitIdle(m_Device);

        // Only the presentation chain dies. The instance, device, allocator and every texture
        // already uploaded survive -- which is the whole reason an Android app does not have to
        // reload its assets every time it is backgrounded.
        m_Swapchain.reset();
        if (m_VkSurface) {
            vkDestroySurfaceKHR(m_Instance, m_VkSurface, nullptr);
            m_VkSurface = VK_NULL_HANDLE;
        }
        m_SurfaceGone = true;
    }

    void VulkanDevice::OnSurfaceRecreated(Surface& surface) {
        m_Surface = &surface;

        const VkResult created = surface.CreateVulkanSurface(m_Instance, &m_VkSurface);
        if (created != VK_SUCCESS) {
            HEARTH_ERROR("could not recreate the presentation surface: {}", VkResultName(created));
            return;
        }

        // The image count can differ from last time, and the per-image semaphores are sized from
        // it, so the chain is rebuilt whole rather than resized.
        if (!BuildSwapchain()) return;
        m_SurfaceGone = false;
    }

    Scope<Device> CreateDevice(const DeviceDesc& desc) {
        auto device = CreateScope<VulkanDevice>(desc);
        if (!device->Ok()) return nullptr;
        return device;
    }

}
