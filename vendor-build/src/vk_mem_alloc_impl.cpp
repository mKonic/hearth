// The single translation unit that compiles VulkanMemoryAllocator.
//
// VMA is a header that contains its own implementation behind this define. Exactly one file in a
// program may define it; hearth claims that file so a consumer never has to, and never ends up
// with two copies fighting over the same symbols.
#define VMA_IMPLEMENTATION

// Dynamic loading: VMA resolves the Vulkan entry points it needs through the two proc-address
// functions VulkanDevice hands it, instead of linking them directly. That keeps hearth working
// against a loader that is dlopened rather than linked, which is how Android ships it.
#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

#include <vk_mem_alloc.h>
