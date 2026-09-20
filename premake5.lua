-- hearth -- a reusable Vulkan device/resource layer.
-- Generate: premake5 gmake   (or run ./scripts/Setup.sh)
include "Dependencies.lua"

workspace "hearth"
   architecture "x86_64"
   startproject "Hearth"
   configurations { "Debug", "Release", "Dist" }
   multiprocessorcompile "On"

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

include "vendor-build/VulkanDeps.lua"
include "Hearth"
include "Hearth-Tests"
