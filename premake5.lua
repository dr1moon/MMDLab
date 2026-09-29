-- Tracy profiler instrumentation is enabled by default: TRACY_ENABLE is defined for every
-- configuration. Pass --no-tracy to premake (or `GenerateProjects.bat --no-tracy`) to compile
-- it out; without TRACY_ENABLE every marker compiles to a no-op.
newoption {
    trigger = "no-tracy",
    description = "Disable Tracy profiler instrumentation (enabled by default)",
}

workspace "MMDLab"
    architecture "x86_64"
    configurations { "Debug", "Release" }
    platforms { "x64" }
    location "."
    startproject "MmdViewer"

    -- Enabled by default across all projects and configurations; disable with --no-tracy.
    if not _OPTIONS["no-tracy"] then
        defines { "TRACY_ENABLE" }
    end

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
    buildoptions { "/utf-8" }
    targetdir "Build/Bin/%{cfg.buildcfg}/%{cfg.platform}"
    objdir "Build/Intermediate/%{prj.name}/%{cfg.buildcfg}/%{cfg.platform}"
    includedirs { "Source", "Source/ThirdParty/tracy" }

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

project "Tracy"
    ConfigureCppProject()
    kind "StaticLib"
    warnings "Off"
    -- Tracy's client uses raw __LINE__ in a constexpr initializer (TracyETW.cpp), which MSVC's
    -- Edit-and-Continue (/ZI, the Debug default) makes non-constant. Compile the client with
    -- plain /Zi instead; the application's own markers are unaffected.
    editandcontinue "Off"
    files {
        "Source/ThirdParty/tracy/TracyClient.cpp",
    }

project "MmdCore"
    ConfigureCppProject()
    kind "StaticLib"
    includedirs {
        "Source/ThirdParty/imgui",
        "Source/ThirdParty/imgui/backends",
    }
    links { "ImGui", "Tracy" }
    files {
        "Source/Runtime/**.h",
        "Source/Runtime/**.hpp",
        "Source/Runtime/**.cpp",
    }
    -- The asset-pipeline (cook) translation units live in MmdCookLib, not the runtime.
    removefiles {
        "Source/Runtime/Asset/TextureCooker.cpp",
        "Source/Runtime/Asset/PmxFile.cpp",
        "Source/Runtime/Asset/MmdlWriter.cpp",
    }

project "MmdCookLib"
    ConfigureCppProject()
    kind "StaticLib"
    files {
        "Source/Runtime/Asset/TextureCooker.cpp",
        "Source/Runtime/Asset/PmxFile.cpp",
        "Source/Runtime/Asset/MmdlWriter.cpp",
        "Source/ThirdParty/bc7enc/bc7enc.cpp",
    }
    links { "Tracy" }

project "MmdViewer"
    ConfigureCppProject()
    kind "ConsoleApp"
    debugdir "%{wks.location}/Project"
    postbuildcommands {
        '{MKDIR} "%{cfg.targetdir}/Fonts"',
        '{COPYFILE} "%{wks.location}/Source/ThirdParty/Fonts/FanWunMing-SB.ttf" "%{cfg.targetdir}/Fonts/FanWunMing-SB.ttf"',
        '{COPYFILE} "%{wks.location}/Source/ThirdParty/Fonts/FanWunMing-LICENSE" "%{cfg.targetdir}/Fonts/FanWunMing-LICENSE"',
    }
    files {
        "Source/App/MmdViewer/**.h",
        "Source/App/MmdViewer/**.hpp",
        "Source/App/MmdViewer/**.cpp",
    }
    links { "MmdCore", "ImGui", "Tracy", "user32", "d3d12", "dxgi", "dxguid", "d3dcompiler" }

project "MmdCooker"
    ConfigureCppProject()
    kind "ConsoleApp"
    files {
        "Source/Tools/MmdCooker/**.h",
        "Source/Tools/MmdCooker/**.hpp",
        "Source/Tools/MmdCooker/**.cpp",
    }
    links { "MmdCookLib", "MmdCore", "Tracy" }

project "MmdTests"
    ConfigureCppProject()
    kind "ConsoleApp"
    files {
        "Source/Tests/**.h",
        "Source/Tests/**.hpp",
        "Source/Tests/**.cpp",
    }
    links { "MmdCore", "MmdCookLib", "Tracy", "d3d12", "dxgi", "dxguid", "d3dcompiler" }
