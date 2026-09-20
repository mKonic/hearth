-- The consuming workspace may or may not define `outputdir`; do not require it to.
local outputdir = outputdir or "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

project "Hearth"
   -- Without this the project would generate into the CONSUMING workspace's directory, and
   -- every relative path in `files` below would resolve against that instead of against
   -- hearth. Pin it to this script's own folder so hearth builds the same wherever it is
   -- vendored.
   location (_SCRIPT_DIR)
   kind "StaticLib"
   language "C++"
   cppdialect "C++23"
   staticruntime "off"
   warnings "Extra"
   -- Every Vulkan struct here is written as `VkFoo x{ VK_STRUCTURE_TYPE_... }`, which
   -- value-initialises the remaining members to zero on purpose. -Wmissing-field-initializers
   -- fires on all several hundred of them and would bury anything real.
   disablewarnings { "missing-field-initializers" }
   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files {
      "src/**.h",
      "src/**.cpp",
   }

   -- The surface adapters are header-only and include GLFW/SDL/Android headers hearth does not
   -- have on its own include path. A consumer includes the one it wants from its own code.
   removefiles { "src/hearth/surface/**" }

   includedirs {
      "%{IncludeDir.Hearth}",
      "%{IncludeDir.VulkanHeaders}",
      "%{IncludeDir.VMA}",
      "%{IncludeDir.vkbootstrap}",
   }

   links { "VulkanDeps", "%{Library.Vulkan}" }

   filter "system:linux"
      pic "On"
      systemversion "latest"

   filter "configurations:Debug"
      runtime "Debug"
      symbols "on"

   filter "configurations:Release"
      runtime "Release"
      optimize "on"
      symbols "on"

   filter "configurations:Dist"
      runtime "Release"
      optimize "full"
      symbols "off"
