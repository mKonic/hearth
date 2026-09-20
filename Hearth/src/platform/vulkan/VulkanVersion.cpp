#include "platform/vulkan/VulkanVersion.h"

#include <string>
#include <vector>

namespace hearth {

    u32 NegotiateInstanceVersion() {
        // Ask the loader rather than declaring a version. Resolved through
        // vkGetInstanceProcAddr and not called directly: on a Vulkan 1.0 loader the entry
        // point does not exist, and a null here is precisely how that reports itself. Calling
        // the linked symbol instead would be a link error on such a machine, or -- worse, and
        // what a plain `if (vkEnumerateInstanceVersion)` actually compiles to -- a test the
        // compiler folds to always-true.
        u32 loader = VK_API_VERSION_1_0;
        const auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
        if (enumerate) {
            if (enumerate(&loader) != VK_SUCCESS) loader = VK_API_VERSION_1_0;
        }

        if (loader < kMinimumApiVersion) {
            HEARTH_ERROR("the Vulkan loader on this machine is {}.{}; hearth needs at least {}.{} "
                         "for dynamic rendering and synchronization2",
                         VK_API_VERSION_MAJOR(loader), VK_API_VERSION_MINOR(loader),
                         VK_API_VERSION_MAJOR(kMinimumApiVersion),
                         VK_API_VERSION_MINOR(kMinimumApiVersion));
            return 0;
        }

        // Clamp to the headers. A loader newer than this build is fine and common -- we just
        // do not claim to speak a version whose structs are not in these headers.
        return std::min(loader, kHighestKnownApiVersion);
    }

    OptionalFeatures ChainOptionalFeatures(VkPhysicalDevice physical, u32 deviceApiVersion,
                                           OptionalFeatureStorage& storage,
                                           std::vector<void*>& outChain) {
        OptionalFeatures found;

        // ---- Vulkan 1.4 ------------------------------------------------------------------
        if (deviceApiVersion >= VK_API_VERSION_1_4
            && kHighestKnownApiVersion >= VK_API_VERSION_1_4) {
            VkPhysicalDeviceVulkan14Features probe{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES };
            VkPhysicalDeviceFeatures2 features2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            features2.pNext = &probe;
            vkGetPhysicalDeviceFeatures2(physical, &features2);

            found.vulkan14      = true;
            found.hostImageCopy = probe.hostImageCopy == VK_TRUE;
            found.pushDescriptor = probe.pushDescriptor == VK_TRUE;
            found.maintenance5  = probe.maintenance5 == VK_TRUE;

            // Enable only what was actually reported. Asking for a feature the device does not
            // have fails device creation outright, which would turn an optional capability
            // into a hard refusal to start.
            storage.v14.hostImageCopy  = probe.hostImageCopy;
            storage.v14.pushDescriptor = probe.pushDescriptor;
            storage.v14.maintenance5   = probe.maintenance5;
            outChain.push_back(&storage.v14);
        }

        return found;
    }

    std::string DescribeOptional(u32 apiVersion, const OptionalFeatures& features) {
        std::string out = std::format("{}.{}", VK_API_VERSION_MAJOR(apiVersion),
                                               VK_API_VERSION_MINOR(apiVersion));
        std::string extras;
        auto add = [&extras](bool present, const char* name) {
            if (!present) return;
            if (!extras.empty()) extras += ", ";
            extras += name;
        };
        add(features.hostImageCopy,  "host-image-copy");
        add(features.pushDescriptor, "push-descriptor");
        add(features.maintenance5,   "maintenance5");

        if (!extras.empty()) out += " (" + extras + ")";
        return out;
    }

}
