#include "App/MmdViewer/WindowsApplication.h"
#include "Runtime/Asset/ImageLoader.h"
#include "Runtime/Asset/MeshAsset.h"
#include "Runtime/Asset/MmdlFile.h"
#include "Runtime/Core/Channel.h"
#include "Runtime/Core/FrameResource.h"
#include "Runtime/Core/FrameResourcePool.h"
#include "Runtime/Core/Thread.h"
#include "Runtime/DX12/RhiThread.h"
#include "Runtime/Render/RenderThread.h"

#include <windows.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
// Converts a UTF-8 byte string (a .mmdl string-table entry) into a wide filesystem path.
std::filesystem::path PathFromUtf8(const std::string& utf8)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), length);
    return std::filesystem::path(wide);
}
} // namespace

int wmain(const int argc, wchar_t* argv[])
{
    try
    {
        if (argc != 2)
        {
            std::wcerr << L"Usage: MmdViewer.exe <model.mmdl>\n";
            return 1;
        }

        // Load the cooked mesh through Runtime/Asset (CPU-side data + draw list).
        const MmdLab::MmdlMeshData meshData = MmdLab::ReadMmdl(argv[1]);
        const MmdLab::MeshAsset mesh = MmdLab::BuildMeshAsset(meshData);

        // Load the model's textures, resolved relative to the .mmdl file's directory.
        const std::filesystem::path baseDirectory = std::filesystem::path(argv[1]).parent_path();
        std::vector<MmdLab::Image> textures;
        textures.reserve(mesh.textures.size());
        for (const std::string& texturePath : mesh.textures)
        {
            textures.push_back(MmdLab::LoadImage(baseDirectory / PathFromUtf8(texturePath)));
        }

        MmdLab::WindowsApplication application;
        application.Initialize(GetModuleHandleW(nullptr), SW_SHOWDEFAULT);

        RECT clientRect{};
        GetClientRect(application.GetWindowHandle(), &clientRect);
        const std::uint32_t width = static_cast<std::uint32_t>(clientRect.right);
        const std::uint32_t height = static_cast<std::uint32_t>(clientRect.bottom);

        // The three-thread pipeline: GameThread (this thread) -> RenderThread -> RhiThread.
        MmdLab::FrameResourcePool pool;
        MmdLab::Channel<MmdLab::FrameIndex, MmdLab::FrameResourcePool::kFrameCount> gameToRender;
        MmdLab::Channel<MmdLab::FrameIndex, MmdLab::FrameResourcePool::kFrameCount> renderToRhi;

        MmdLab::RenderThread renderStage(gameToRender, renderToRhi, pool, mesh);
        MmdLab::RhiThread rhiStage(renderToRhi, pool, application.GetWindowHandle(), width, height, mesh, textures);

        MmdLab::Thread renderThread(renderStage, L"RenderThread");
        MmdLab::Thread rhiThread(rhiStage, L"RhiThread");

        // GameThread role: produce one frame per iteration, throttled by the frame pool.
        MmdLab::FrameId frameId = 0;
        while (application.ProcessMessages())
        {
            const MmdLab::FrameIndex index = pool.Acquire();
            pool.Get(index).frameId = ++frameId;
            gameToRender.Push(index);
        }

        // Shut down upstream-first so the pipeline drains in order.
        renderThread.RequestStop();
        rhiThread.RequestStop();

        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "MMDLab Viewer failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
