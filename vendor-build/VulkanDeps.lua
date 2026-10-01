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

   -- As system headers: this project is only third-party code, and its warnings are not ours.
   externalincludedirs {
      "%{IncludeDir.VulkanHeaders}",
      "%{IncludeDir.VMA}",
      "%{IncludeDir.vkbootstrap}",
   }

   filter "system:linux"
      pic "On"
      systemversion "latest"

   -- An Android app is a shared library, so everything linked into it must be PIC. The
   -- platform defines are what vk-bootstrap reads to enable the surface extension.
   filter "system:android"
      pic "On"
      defines { "VK_USE_PLATFORM_ANDROID_KHR" }

   filter "system:macosx or system:ios"
      defines { "VK_USE_PLATFORM_METAL_EXT" }
