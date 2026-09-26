#include "App/MmdViewer/Scene.h"

#include "Runtime/Asset/PmxFile.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
// Converts a UTF-8 string (a PMX string-table entry) into a wide filesystem path.
std::filesystem::path PathFromUtf8(const std::string& utf8)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), length);
    return std::filesystem::path(wide);
}

// Converts a wide filename into a UTF-8 display string.
std::string WideToUtf8(const std::wstring& wide)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

MmdLab::Image WhiteFallbackImage()
{
    MmdLab::Image image;
    image.width = 1;
    image.height = 1;
    image.pixels = { 255, 255, 255, 255 };
    return image;
}
} // namespace

namespace MmdLab
{
void Scene::LoadFromDirectory(const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> pmxFiles;
    if (std::filesystem::is_directory(directory))
    {
        // Recursive so a project can nest each model in its own subfolder (Models/<model>/*.pmx)
        // while keeping that model's textures/ beside its .pmx.
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
        {
            if (entry.is_regular_file() && entry.path().extension() == L".pmx")
            {
                pmxFiles.push_back(entry.path());
            }
        }
    }
    std::sort(pmxFiles.begin(), pmxFiles.end());

    for (const std::filesystem::path& path : pmxFiles)
    {
        try
        {
            const PmxStaticMesh pmx = ParsePmxStaticMesh(path);
            const MmdlMeshData meshData = ConvertPmxToMmdl(pmx);

            SceneModel model;
            model.mesh = BuildMeshAsset(meshData);
            const std::string displayName = WideToUtf8(path.stem().wstring());

            // Load the model's textures, resolved relative to the .pmx file's directory. A
            // missing texture is replaced with white so material texture indices stay aligned.
            const std::filesystem::path baseDirectory = path.parent_path();
            model.textures.reserve(model.mesh.textures.size());
            for (const std::string& texturePath : model.mesh.textures)
            {
                try
                {
                    model.textures.push_back(DecodeImage(baseDirectory / PathFromUtf8(texturePath)));
                }
                catch (const std::exception& exception)
                {
                    std::cerr << "Missing texture '" << texturePath << "' for " << displayName
                              << ": " << exception.what() << " (using white fallback).\n";
                    model.textures.push_back(WhiteFallbackImage());
                }
            }

            models_.push_back(std::move(model));
            displayNames_.push_back(displayName);
        }
        catch (const std::exception& exception)
        {
            std::cerr << "Failed to load PMX model '" << path << "': " << exception.what() << '\n';
        }
    }
}

void Scene::Select(const std::size_t index)
{
    if (index >= models_.size() || index == selectedModel_)
    {
        return;
    }
    selectedModel_ = index;
    ++modelGeneration_;
}

const MeshAsset* Scene::SelectedMesh() const
{
    if (models_.empty())
    {
        return nullptr;
    }
    return &models_[selectedModel_].mesh;
}

std::span<const Image> Scene::SelectedTextures() const
{
    if (models_.empty())
    {
        return {};
    }
    return models_[selectedModel_].textures;
}
} // namespace MmdLab
