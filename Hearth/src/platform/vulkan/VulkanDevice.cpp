#include "platform/vulkan/VulkanDevice.h"

#include "platform/vulkan/VulkanCommandList.h"
#include "platform/vulkan/VulkanResources.h"
#include "platform/vulkan/VulkanSwapchain.h"
#include "platform/vulkan/VulkanVersion.h"

#include <VkBootstrap.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
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
        for (auto& [key, pass] : m_RenderPasses) vkDestroyRenderPass(m_Device, pass, nullptr);
        if (m_ImmediateFence) vkDestroyFence(m_Device, m_ImmediateFence, nullptr);
        if (m_ImmediatePool)  vkDestroyCommandPool(m_Device, m_ImmediatePool, nullptr);
        for (auto pool : m_DescriptorPools) vkDestroyDescriptorPool(m_Device, pool, nullptr);
        if (m_Allocator)  vmaDestroyAllocator(m_Allocator);
        if (m_Device)     vkDestroyDevice(m_Device, nullptr);
        if (m_VkSurface)  vkDestroySurfaceKHR(m_Instance, m_VkSurface, nullptr);
        if (m_Messenger)  vkb::destroy_debug_utils_messenger(m_Instance, m_Messenger);
        if (m_Instance)   vkDestroyInstance(m_Instance, nullptr);
    }

    namespace {

        // HEARTH_COMPAT: a comma-separated list that switches off what a device offers, so the
        // paths an older phone takes can be exercised on any machine (the test suite runs once
        // per path). "extension" uses VK_KHR_dynamic_rendering even on a 1.3 device,
        // "renderpass" uses VkRenderPass objects, "no-indexing" leaves descriptor indexing off.
        struct Compat {
            bool extension = false;
            bool renderPass = false;
            bool noIndexing = false;
        };

        Compat ReadCompat() {
            Compat compat;
            const char* env = std::getenv("HEARTH_COMPAT");
            if (!env || !*env) return compat;
            const std::string list = env;
            auto has = [&](std::string_view word) {
                size_t start = 0;
                while (start <= list.size()) {
                    const size_t end = std::min(list.find(',', start), list.size());
                    if (std::string_view(list).substr(start, end - start) == word) return true;
                    start = end + 1;
                }
                return false;
            };
            compat.extension  = has("extension");
            compat.renderPass = has("renderpass");
            compat.noIndexing = has("no-indexing");
            HEARTH_WARN("HEARTH_COMPAT={}: features are being withheld on purpose", list);
            return compat;
        }

        bool HasExtension(VkPhysicalDevice physical, const char* name) {
            u32 count = 0;
            vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> extensions(count);
            vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());
            for (const auto& e : extensions)
                if (std::strcmp(e.extensionName, name) == 0) return true;
            return false;
        }

        std::string VersionString(u32 version) {
            return std::format("{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version));
        }

    }

    bool VulkanDevice::Fail(DeviceErrorCode code, std::string message,
                            std::vector<std::string> missing) {
        HEARTH_ERROR("{}", message);
        m_Error.code = code;
        m_Error.message = std::move(message);
        m_Error.missing = std::move(missing);
        return false;
    }

    // Instance, surface and device are one step: vk-bootstrap's PhysicalDeviceSelector needs the
    // live vkb::Instance -- its api version, its enabled extensions and its loader function
    // pointers -- and none of that can be rebuilt from a bare VkInstance handle afterwards.
    bool VulkanDevice::InitVulkan(const DeviceDesc& desc) {
        m_Validation = desc.enableValidation;
        const Compat compat = ReadCompat();

        // `request_validation_layers` is a request, not a requirement: when the layers are not
        // installed vk-bootstrap quietly builds the instance without them and succeeds. Asking
        // the system first is the only way to tell the difference, and the difference matters
        // -- "validation on" in the log when nothing is validating is worse than no message,
        // because it is the line someone reads before concluding their code is clean.
        if (m_Validation) {
            auto systemInfo = vkb::SystemInfo::get_system_info();
            if (!systemInfo || !systemInfo->validation_layers_available) {
                HEARTH_WARN("validation was requested but the layers are not installed "
                            "(Arch: vulkan-validation-layers); continuing without them");
                m_Validation = false;
            }
        }

        const u32 instanceVersion = NegotiateInstanceVersion();
        if (instanceVersion == 0)
            return Fail(DeviceErrorCode::LoaderTooOld,
                        std::format("the Vulkan loader is older than {}",
                                    VersionString(kMinimumApiVersion)));

        const u32 floor = std::max(desc.minimumApiVersion, kMinimumApiVersion);
        if (instanceVersion < floor)
            return Fail(DeviceErrorCode::LoaderTooOld,
                        std::format("this application asked for Vulkan {} but the loader offers {}",
                                    VersionString(floor), VersionString(instanceVersion)));

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
        if (!result)
            return Fail(DeviceErrorCode::InstanceFailed,
                        std::format("vulkan instance creation failed: {}", result.error().message()));

        vkb::Instance instance = result.value();
        m_Instance  = instance.instance;
        m_Messenger = instance.debug_messenger;

        if (m_Surface) {
            const VkResult created = m_Surface->CreateVulkanSurface(m_Instance, &m_VkSurface);
            if (created != VK_SUCCESS)
                return Fail(DeviceErrorCode::SurfaceFailed,
                            std::format("the host could not create a presentation surface: {}",
                                        VkResultName(created)));
        }

        // Descriptor indexing through the struct that was promoted into 1.2 rather than
        // VkPhysicalDeviceVulkan12Features, so the same request works on a 1.1 driver with
        // VK_EXT_descriptor_indexing. partiallyBound is what makes an array binding legal to
        // leave half-written; non-uniform indexing is what a batcher's shader does.
        VkPhysicalDeviceDescriptorIndexingFeatures indexing{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
        indexing.runtimeDescriptorArray = VK_TRUE;
        indexing.descriptorBindingPartiallyBound = VK_TRUE;
        indexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

        // Select on the floor alone, so the best GPU wins (vk-bootstrap prefers discrete), and
        // then take the best rendering path that GPU offers -- rather than settling for a weaker
        // GPU because it happens to have a newer driver.
        vkb::PhysicalDeviceSelector selector{ instance };
        selector.set_minimum_version(floor);
        if (desc.requireDescriptorIndexing) selector.add_required_extension_features(indexing);
        if (m_VkSurface) selector.set_surface(m_VkSurface);
        else             selector.defer_surface_initialization();

        auto physical = selector.select();
        if (!physical) {
            std::vector<std::string> missing = DescribeMissing(floor, desc.requireDescriptorIndexing);
            std::string list;
            for (const auto& m : missing) list += (list.empty() ? "" : ", ") + m;
            return Fail(DeviceErrorCode::NoSuitableDevice,
                        std::format("no suitable Vulkan device{}{} ({})",
                                    list.empty() ? "" : ": missing ", list,
                                    physical.error().message()),
                        std::move(missing));
        }

        vkb::PhysicalDevice chosen = physical.value();

        VkPhysicalDeviceProperties selected{};
        vkGetPhysicalDeviceProperties(chosen.physical_device, &selected);
        // The usable version is the lowest of the three parties: loader, headers, device.
        m_ApiVersion = std::min(instanceVersion, selected.apiVersion);

        // ---- the rendering path ------------------------------------------------------------
        bool dynamicRendering = false;
        const char* path = "render passes";
        if (m_ApiVersion >= VK_API_VERSION_1_3 && !compat.extension && !compat.renderPass) {
            VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
            f13.dynamicRendering = VK_TRUE;
            if (chosen.enable_extension_features_if_present(f13)) {
                dynamicRendering = true;
                m_RenderingFns.begin = reinterpret_cast<PFN_vkCmdBeginRendering>(
                    vkGetInstanceProcAddr(m_Instance, "vkCmdBeginRendering"));
                m_RenderingFns.end = reinterpret_cast<PFN_vkCmdEndRendering>(
                    vkGetInstanceProcAddr(m_Instance, "vkCmdEndRendering"));
                path = "dynamic rendering";
            }
        }
        if (!dynamicRendering && !compat.renderPass
            && HasExtension(chosen.physical_device, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
            // Its dependencies are core from 1.2; a 1.1 driver lists them as extensions.
            std::vector<const char*> needed = { VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME };
            if (m_ApiVersion < VK_API_VERSION_1_2) {
                needed.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
                needed.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
                needed.push_back(VK_KHR_MULTIVIEW_EXTENSION_NAME);
                needed.push_back(VK_KHR_MAINTENANCE_2_EXTENSION_NAME);
            }
            VkPhysicalDeviceDynamicRenderingFeaturesKHR dr{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR };
            dr.dynamicRendering = VK_TRUE;
            if (chosen.enable_extensions_if_present(needed)
                && chosen.enable_extension_features_if_present(dr)) {
                dynamicRendering = true;
                m_RenderingFns.begin = reinterpret_cast<PFN_vkCmdBeginRendering>(
                    vkGetInstanceProcAddr(m_Instance, "vkCmdBeginRenderingKHR"));
                m_RenderingFns.end = reinterpret_cast<PFN_vkCmdEndRendering>(
                    vkGetInstanceProcAddr(m_Instance, "vkCmdEndRenderingKHR"));
                path = "dynamic rendering (KHR)";
            }
        }
        if (dynamicRendering && (!m_RenderingFns.begin || !m_RenderingFns.end))
            return Fail(DeviceErrorCode::DeviceFailed,
                        "dynamic rendering was enabled but its entry points did not resolve");

        // ---- optional features ---------------------------------------------------------------
        // Every one of these is required of a Vulkan 1.3 device, so on one this narrows nothing.
        // They are enabled because a shader that declares the matching SPIR-V capability --
        // which glslc emits for `discard` when targeting 1.3 -- fails vkCreateShaderModule if
        // the feature is off, and the error names the capability, not the line of GLSL.
        // Not alongside the KHR dynamic-rendering struct: the spec forbids chaining a feature
        // struct together with the VkPhysicalDeviceVulkan13Features that contains it.
        const bool khrStruct = dynamicRendering && m_ApiVersion >= VK_API_VERSION_1_3
                            && std::string_view(path) != "dynamic rendering";
        if (m_ApiVersion >= VK_API_VERSION_1_3 && !khrStruct) {
            VkPhysicalDeviceVulkan13Features shader13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
            shader13.shaderDemoteToHelperInvocation      = VK_TRUE;
            shader13.shaderTerminateInvocation           = VK_TRUE;
            shader13.shaderZeroInitializeWorkgroupMemory = VK_TRUE;
            shader13.subgroupSizeControl                 = VK_TRUE;
            shader13.computeFullSubgroups                = VK_TRUE;
            shader13.maintenance4                        = VK_TRUE;
            chosen.enable_extension_features_if_present(shader13);
        }

        const bool indexingExtension = m_ApiVersion < VK_API_VERSION_1_2;
        if (!compat.noIndexing
            && (!indexingExtension
                || chosen.enable_extension_if_present(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME))) {
            m_Caps.descriptorIndexing = chosen.enable_extension_features_if_present(indexing);
            // What makes VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT legal on a sampler array,
            // and what makes rewriting one that is already bound legal. Only the sampled-image
            // variants: the uniform-buffer one is genuinely absent on some hardware.
            VkPhysicalDeviceDescriptorIndexingFeatures afterBind{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
            afterBind.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            afterBind.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
            m_Caps.updateAfterBind = m_Caps.descriptorIndexing
                                  && chosen.enable_extension_features_if_present(afterBind);
        }
        if (desc.requireDescriptorIndexing && !m_Caps.descriptorIndexing)
            return Fail(DeviceErrorCode::NoSuitableDevice,
                        "descriptor indexing was required and is unavailable",
                        { "descriptor indexing" });

        // Anisotropic filtering is optional in the spec, so it is enabled if the device has
        // it rather than required: a device without it still runs, and Caps().maxAnisotropy
        // reports 1 so a caller can tell.
        VkPhysicalDeviceFeatures optionalCore{};
        optionalCore.samplerAnisotropy = VK_TRUE;
        const bool anisotropy = chosen.enable_features_if_present(optionalCore);
        VkPhysicalDeviceFeatures biasClamp{};
        biasClamp.depthBiasClamp = VK_TRUE;
        m_Caps.depthBiasClamp = chosen.enable_features_if_present(biasClamp);

        OptionalFeatureStorage optionalStorage;
        std::vector<void*> optionalChain;
        const OptionalFeatures optional = ChainOptionalFeatures(
            chosen.physical_device, m_ApiVersion, optionalStorage, optionalChain);

        vkb::DeviceBuilder deviceBuilder{ chosen };
        for (void* link : optionalChain) deviceBuilder.add_pNext(link);

        auto built = deviceBuilder.build();
        if (!built)
            return Fail(DeviceErrorCode::DeviceFailed,
                        std::format("logical device creation failed: {}", built.error().message()));

        m_Physical       = chosen.physical_device;
        m_Device         = built.value().device;
        m_GraphicsQueue  = built.value().get_queue(vkb::QueueType::graphics).value();
        m_GraphicsFamily = built.value().get_queue_index(vkb::QueueType::graphics).value();
        m_Caps.dynamicRendering = dynamicRendering;

        // The plain maxPerStageDescriptorSampledImages is the wrong limit to report: an array
        // binding a batcher rewrites while it is bound needs UPDATE_AFTER_BIND, and that has its
        // own, usually much larger, cap.
        VkPhysicalDeviceDescriptorIndexingProperties indexingProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES };
        VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        if (m_Caps.descriptorIndexing) props2.pNext = &indexingProps;
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
        m_Caps.sampleCountMask = static_cast<u32>(props.limits.framebufferColorSampleCounts
                                                & props.limits.framebufferDepthSampleCounts)
                               | 1u;   // 1x is always available
        for (u32 count : { 64u, 32u, 16u, 8u, 4u, 2u }) {
            if (m_Caps.sampleCountMask & count) { m_Caps.maxSamples = count; break; }
        }
        m_Caps.maxTexturesPerBindGroup = props.limits.maxPerStageDescriptorSampledImages;
        if (m_Caps.updateAfterBind)
            m_Caps.maxTexturesPerBindGroup =
                std::max(indexingProps.maxDescriptorSetUpdateAfterBindSampledImages,
                         m_Caps.maxTexturesPerBindGroup);
        m_Caps.apiVersion       = m_ApiVersion;
        m_Caps.validationActive = m_Validation;
        m_Caps.hostImageCopy  = optional.hostImageCopy;
        m_Caps.pushDescriptor = optional.pushDescriptor;
        m_Caps.maintenance5   = optional.maintenance5;
        m_Caps.driverInfo     = std::format("Vulkan {}, {}{}", DescribeOptional(m_ApiVersion, optional),
                                            path, m_Caps.descriptorIndexing ? ", descriptor indexing" : "");

        HEARTH_INFO("{} ({}){}, {}", m_Caps.deviceName,
                    m_Caps.discrete ? "discrete" : (m_Caps.softwareRasterizer ? "CPU" : "integrated"),
                    m_Validation ? ", validation on" : "", m_Caps.driverInfo);
        return true;
    }

    // Why selection found nothing, for the device that came closest: the one with the highest
    // version. Asked of the driver directly, because vk-bootstrap reports only the first reason
    // it rejected each device and not in a form a caller can show.
    std::vector<std::string> VulkanDevice::DescribeMissing(u32 floor, bool needIndexing) const {
        u32 count = 0;
        vkEnumeratePhysicalDevices(m_Instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(m_Instance, &count, devices.data());
        if (devices.empty()) return { "a Vulkan driver" };

        VkPhysicalDevice best = devices.front();
        VkPhysicalDeviceProperties bestProps{};
        vkGetPhysicalDeviceProperties(best, &bestProps);
        for (VkPhysicalDevice d : devices) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(d, &p);
            if (p.apiVersion > bestProps.apiVersion) { best = d; bestProps = p; }
        }

        std::vector<std::string> missing;
        if (bestProps.apiVersion < floor) missing.push_back("Vulkan " + VersionString(floor));

        if (needIndexing) {
            VkPhysicalDeviceDescriptorIndexingFeatures f{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
            VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            f2.pNext = &f;
            vkGetPhysicalDeviceFeatures2(best, &f2);
            if (!f.runtimeDescriptorArray || !f.descriptorBindingPartiallyBound
                || !f.shaderSampledImageArrayNonUniformIndexing)
                missing.push_back("descriptor indexing");
        }

        u32 families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(best, &families, nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        vkGetPhysicalDeviceQueueFamilyProperties(best, &families, queues.data());
        bool graphics = false, present = !m_VkSurface;
        for (u32 i = 0; i < families; ++i) {
            if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) graphics = true;
            if (m_VkSurface) {
                VkBool32 supported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(best, i, m_VkSurface, &supported);
                present = present || supported;
            }
        }
        if (!graphics) missing.push_back("a graphics queue");
        if (!present)  missing.push_back("presentation to this surface");
        return missing;
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
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        if (m_Caps.updateAfterBind) info.flags |= VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
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

    // Only on the render-pass path. One VkRenderPass per attachment layout and load op; a
    // pipeline is built against the Clear variant, which every other variant with the same
    // formats and sample count is compatible with.
    //
    // Attachments are numbered colours first, then (multisampled) their resolve targets, then
    // depth. Every layout is the one the pass uses throughout: CommandList moves images into
    // and out of it with its own barriers, exactly as on the dynamic-rendering path, so the
    // pass itself transitions nothing.
    VkRenderPass VulkanDevice::RenderPassFor(const std::vector<VkFormat>& colors, VkFormat depth,
                                             u32 samples, LoadOp load) {
        const std::scoped_lock lock(m_RenderPassMutex);
        for (const auto& [key, pass] : m_RenderPasses)
            if (key.colors == colors && key.depth == depth && key.samples == samples && key.load == load)
                return pass;

        const VkSampleCountFlagBits sampleBits = static_cast<VkSampleCountFlagBits>(samples);
        const bool resolve = samples > 1;
        const VkAttachmentLoadOp loadOp = load == LoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                        : load == LoadOp::Load  ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                                : VK_ATTACHMENT_LOAD_OP_DONT_CARE;

        std::vector<VkAttachmentDescription> attachments;
        std::vector<VkAttachmentReference> colorRefs, resolveRefs;
        for (VkFormat format : colors) {
            VkAttachmentDescription a{};
            a.format = format;
            a.samples = sampleBits;
            a.loadOp = loadOp;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorRefs.push_back({ static_cast<u32>(attachments.size()),
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
            attachments.push_back(a);
        }
        if (resolve) {
            for (VkFormat format : colors) {
                VkAttachmentDescription a{};
                a.format = format;
                a.samples = VK_SAMPLE_COUNT_1_BIT;
                a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                resolveRefs.push_back({ static_cast<u32>(attachments.size()),
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
                attachments.push_back(a);
            }
        }
        VkAttachmentReference depthRef{};
        if (depth != VK_FORMAT_UNDEFINED) {
            VkAttachmentDescription a{};
            a.format = depth;
            a.samples = sampleBits;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;       // depth is cleared by every pass
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthRef = { static_cast<u32>(attachments.size()),
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
            attachments.push_back(a);
        }

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = static_cast<u32>(colorRefs.size());
        subpass.pColorAttachments = colorRefs.data();
        subpass.pResolveAttachments = resolve ? resolveRefs.data() : nullptr;
        subpass.pDepthStencilAttachment = depth != VK_FORMAT_UNDEFINED ? &depthRef : nullptr;

        VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        info.attachmentCount = static_cast<u32>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;

        VkRenderPass pass = VK_NULL_HANDLE;
        HEARTH_VK_CHECK(vkCreateRenderPass(m_Device, &info, nullptr, &pass));
        m_RenderPasses.push_back({ RenderPassKey{ colors, depth, samples, load }, pass });
        return pass;
    }

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

        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        HEARTH_VK_CHECK(vkQueueSubmit(m_GraphicsQueue, 1, &submit, m_ImmediateFence));
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
            ImageBarrier(frame.cmd, m_Swapchain->Image(m_ImageIndex), from,
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
        }

        HEARTH_VK_CHECK(vkEndCommandBuffer(frame.cmd));
        m_CommandList->End();

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore renderFinished = m_Swapchain ? m_Swapchain->RenderFinished(m_ImageIndex)
                                                 : VK_NULL_HANDLE;
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.cmd;
        if (m_Swapchain) {
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &frame.imageAvailable;
            submit.pWaitDstStageMask = &waitStage;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &renderFinished;
        }
        HEARTH_VK_CHECK(vkQueueSubmit(m_GraphicsQueue, 1, &submit, frame.inFlight));

        if (m_Swapchain) {
            VkSwapchainKHR chain = m_Swapchain->Raw();
            VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &renderFinished;
            present.swapchainCount = 1;
            present.pSwapchains = &chain;
            present.pImageIndices = &m_ImageIndex;

            const VkResult presented = vkQueuePresentKHR(m_GraphicsQueue, &present);
            // SUBOPTIMAL alone is not a reason to rebuild: Android reports it on every present
            // while the compositor rotates for us (an IDENTITY preTransform on a turned
            // display), and rebuilding would not change that. Rebuild when the size moved.
            bool sizeChanged = presented == VK_ERROR_OUT_OF_DATE_KHR;
            if (presented == VK_SUBOPTIMAL_KHR) {
                VkSurfaceCapabilitiesKHR caps{};
                if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_Physical, m_VkSurface, &caps) == VK_SUCCESS
                    && caps.currentExtent.width != UINT32_MAX)
                    sizeChanged = caps.currentExtent.width != m_Swapchain->Width()
                               || caps.currentExtent.height != m_Swapchain->Height();
                else
                    sizeChanged = true;
            }
            if (sizeChanged) {
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

    const char* DeviceErrorName(DeviceErrorCode code) {
        switch (code) {
            case DeviceErrorCode::None:             return "none";
            case DeviceErrorCode::LoaderTooOld:     return "loader too old";
            case DeviceErrorCode::InstanceFailed:   return "instance creation failed";
            case DeviceErrorCode::SurfaceFailed:    return "surface creation failed";
            case DeviceErrorCode::NoSuitableDevice: return "no suitable device";
            case DeviceErrorCode::DeviceFailed:     return "device creation failed";
        }
        return "unknown";
    }

    Scope<Device> CreateDevice(const DeviceDesc& desc, DeviceError* error) {
        auto device = CreateScope<VulkanDevice>(desc);
        if (error) *error = device->Error();
        if (!device->Ok()) {
            // A failure past device selection (an allocator, a command pool) has no code yet.
            if (error && error->code == DeviceErrorCode::None) {
                error->code = DeviceErrorCode::DeviceFailed;
                error->message = "device initialisation failed after creation";
            }
            return nullptr;
        }
        return device;
    }

}
