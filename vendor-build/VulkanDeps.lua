-- vk-bootstrap and VMA compiled into their own static library.
--
-- They are third-party and warn heavily under the flags hearth builds its own code with, so
-- isolating them keeps hearth's build output readable. A consumer links this alongside Hearth.
-- The consuming workspace may or may not define `outputdir`; do not require it to.
local outputdir = outputdir or "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

project "VulkanDeps"
   -- Without this the project would generate into the CONSUMING workspace's directory, and
   -- every relative path in `files` below would resolve against that instead of against
   -- hearth. Pin it to this script's own folder so hearth builds the same wherever it is
   -- vendored.
   location (_SCRIPT_DIR)
   kind "StaticLib"
   language "C++"
   cppdialect "C++20"
   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files {
      "../vendor/vk-bootstrap/src/VkBootstrap.cpp",
      "../vendor/vk-bootstrap/src/VkBootstrap.h",
      "src/vk_mem_alloc_impl.cpp",
   }

   includedirs {
      "%{IncludeDir.VulkanHeaders}",
      "%{IncludeDir.VMA}",
      "%{IncludeDir.vkbootstrap}",
   }

   filter "system:linux"
      pic "On"
      systemversion "latest"
