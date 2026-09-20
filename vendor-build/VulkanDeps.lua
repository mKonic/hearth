-- vk-bootstrap and VMA compiled into their own static library.
--
-- They are third-party and warn heavily under the flags hearth builds its own code with, so
-- isolating them keeps hearth's build output readable. A consumer links this alongside Hearth.
-- The consuming workspace may or may not define `outputdir`; do not require it to.
local outputdir = outputdir or "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

project "VulkanDeps"
   kind "StaticLib"
   language "C++"
   cppdialect "C++20"
   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files {
      HearthRoot .. "/vendor/vk-bootstrap/src/VkBootstrap.cpp",
      HearthRoot .. "/vendor/vk-bootstrap/src/VkBootstrap.h",
      HearthRoot .. "/vendor-build/src/vk_mem_alloc_impl.cpp",
   }

   includedirs {
      "%{IncludeDir.VulkanHeaders}",
      "%{IncludeDir.VMA}",
      "%{IncludeDir.vkbootstrap}",
   }

   filter "system:linux"
      pic "On"
      systemversion "latest"
