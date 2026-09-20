-- The consuming workspace may or may not define `outputdir`; do not require it to.
local outputdir = outputdir or "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

project "Hearth-Tests"
   -- Without this the project would generate into the CONSUMING workspace's directory, and
   -- every relative path in `files` below would resolve against that instead of against
   -- hearth. Pin it to this script's own folder so hearth builds the same wherever it is
   -- vendored.
   location (_SCRIPT_DIR)
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++23"
   staticruntime "off"
   warnings "Extra"
   disablewarnings { "missing-field-initializers" }
   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files { "src/**.h", "src/**.cpp" }

   includedirs {
      "src",
      "%{IncludeDir.Hearth}",
      "%{IncludeDir.VulkanHeaders}",
   }

   links { "Hearth", "VulkanDeps", "%{Library.Vulkan}" }

   filter "system:linux"
      pic "On"
      systemversion "latest"

   filter "configurations:Debug"
      runtime "Debug"
      symbols "on"
