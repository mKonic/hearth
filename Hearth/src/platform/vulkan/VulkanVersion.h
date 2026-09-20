#pragma once

#include "platform/vulkan/VulkanCommon.h"

// The only file in hearth that names a Vulkan core version.
//
// **Adding a version when Khronos ships it:** bump `vendor/Vulkan-Headers` and
// `vendor/vk-bootstrap`, then add one block to `ChainOptionalFeatures` in VulkanVersion.cpp
// and one line to `DescribeOptional`. Nothing else changes -- the negotiation below already
// picks up the higher version on its own, because it asks the loader and the device rather
// than carrying a constant.

namespace hearth {

    // hearth's architectural floor. Dynamic rendering and synchronization2 are core in 1.3
    // and the whole backend is built on them, so there is no meaningful 1.2 path; a device
    // below this is rejected rather than silently given a slower one.
    inline constexpr u32 kMinimumApiVersion = VK_API_VERSION_1_3;

    // The highest version these headers can describe. Requesting above it would name feature
    // structs the compiler has never seen, so a newer loader is deliberately clamped down to
    // here until the submodule is bumped.
    inline constexpr u32 kHighestKnownApiVersion = VK_HEADER_VERSION_COMPLETE;

    // Highest version the loader on this machine offers, clamped to what these headers know.
    // Zero when the loader cannot even meet kMinimumApiVersion.
    u32 NegotiateInstanceVersion();

    // Core features this device has beyond the floor, and which of them hearth switched on.
    struct OptionalFeatures {
        bool vulkan14 = false;
        // 1.4. Not yet used by any hearth code path -- reported so a consumer can branch, and
        // so that adopting it is a change in one place rather than a plumbing exercise.
        bool hostImageCopy = false;
        bool pushDescriptor = false;
        bool maintenance5 = false;
    };

    // Inspects the selected physical device and appends whatever optional feature structs it
    // supports to the device-creation chain. `storage` must outlive the DeviceBuilder call.
    struct OptionalFeatureStorage {
        VkPhysicalDeviceVulkan14Features v14{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES };
    };

    OptionalFeatures ChainOptionalFeatures(VkPhysicalDevice physical, u32 deviceApiVersion,
                                           OptionalFeatureStorage& storage,
                                           std::vector<void*>& outChain);

    // One line for the startup log, e.g. "1.4 (host-image-copy, push-descriptor)".
    std::string DescribeOptional(u32 apiVersion, const OptionalFeatures& features);

}
