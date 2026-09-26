workspace "MMDLab"
    architecture "x86_64"
    configurations { "Debug", "Release" }
    platforms { "x64" }
    location "."
    startproject "MmdViewer"

    filter "platforms:x64"
        architecture "x86_64"

    filter "configurations:Debug"
        defines { "MMDLAB_DEBUG" }
        runtime "Debug"
        symbols "On"

    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "Speed"
        runtime "Release"

    filter {}

local function ConfigureCppProject()
    location "Build/VisualStudio"
    language "C++"
    cppdialect "C++latest"
    systemversion "latest"
    warnings "Extra"
    characterset "Unicode"
    targetdir "Build/Bin/%{cfg.buildcfg}/%{cfg.platform}"
    objdir "Build/Intermediate/%{prj.name}/%{cfg.buildcfg}/%{cfg.platform}"
    includedirs { "Source" }

    filter "system:windows"
        defines { "UNICODE", "_UNICODE", "WIN32_LEAN_AND_MEAN", "NOMINMAX" }
end

project "ImGui"
    ConfigureCppProject()
    kind "StaticLib"
    includedirs {
        "Source/ThirdParty/imgui",
        "Source/ThirdParty/imgui/backends",
    }
    files {
        "Source/ThirdParty/imgui/imgui.cpp",
        "Source/ThirdParty/imgui/imgui_draw.cpp",
        "Source/ThirdParty/imgui/imgui_tables.cpp",
        "Source/ThirdParty/imgui/imgui_widgets.cpp",
        "Source/ThirdParty/imgui/backends/imgui_impl_win32.cpp",
        "Source/ThirdParty/imgui/backends/imgui_impl_dx12.cpp",
    }

project "MmdRuntime"
    ConfigureCppProject()
    kind "StaticLib"
    includedirs {
        "Source/ThirdParty/imgui",
        "Source/ThirdParty/imgui/backends",
    }
    links { "ImGui" }
    files {
        "Source/Runtime/**.h",
        "Source/Runtime/**.hpp",
        "Source/Runtime/**.cpp",
    }

project "MmdViewer"
    ConfigureCppProject()
    kind "ConsoleApp"
    files {
        "Source/App/MmdViewer/**.h",
        "Source/App/MmdViewer/**.hpp",
        "Source/App/MmdViewer/**.cpp",
    }
    links { "MmdRuntime", "ImGui", "user32", "d3d12", "dxgi", "dxguid", "d3dcompiler" }

project "MmdCooker"
    ConfigureCppProject()
    kind "ConsoleApp"
    files {
        "Source/Tools/MmdCooker/**.h",
        "Source/Tools/MmdCooker/**.hpp",
        "Source/Tools/MmdCooker/**.cpp",
    }
    links { "MmdRuntime" }

project "MmdTests"
    ConfigureCppProject()
    kind "ConsoleApp"
    files {
        "Source/Tests/**.h",
        "Source/Tests/**.hpp",
        "Source/Tests/**.cpp",
    }
    links { "MmdRuntime", "d3d12", "dxgi", "dxguid" }
