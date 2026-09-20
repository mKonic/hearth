-- Include-dir and library tables for hearth.
--
-- The Vulkan loader comes from the system (vulkan-icd-loader). Headers, VMA and vk-bootstrap
-- are vendored as submodules so a consumer pins the same versions hearth was built against --
-- VMA in particular is a single header whose allocation behaviour changes between releases.
--
-- Nothing windowing-related appears here on purpose. hearth links no window library; see
-- Hearth/src/hearth/Surface.h.

-- Absolute, and worked out from where this file actually is. A consumer that vendored hearth
-- at some other depth does not have to tell it so, and these paths stay valid no matter which
-- workspace is including them.
HearthRoot = _SCRIPT_DIR

IncludeDir = IncludeDir or {}
IncludeDir["Hearth"]        = HearthRoot .. "/Hearth/src"
IncludeDir["VulkanHeaders"] = HearthRoot .. "/vendor/Vulkan-Headers/include"
IncludeDir["VMA"]           = HearthRoot .. "/vendor/VulkanMemoryAllocator/include"
IncludeDir["vkbootstrap"]   = HearthRoot .. "/vendor/vk-bootstrap/src"

Library = Library or {}
Library["Vulkan"] = "vulkan"    -- Linux loader; "vulkan-1" on Windows
