#include "Runtime/DX12/RenderDocCapture.h"

namespace MmdLab
{
RenderDocCapture::RenderDocCapture(const wchar_t* dllPath)
{
    module_ = LoadLibraryW(dllPath != nullptr ? dllPath : L"renderdoc.dll");
    if (module_ == nullptr)
    {
        return;
    }

    const auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(
        GetProcAddress(module_, "RENDERDOC_GetAPI"));
    if (getApi == nullptr)
    {
        FreeLibrary(module_);
        module_ = nullptr;
        return;
    }

    if (getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&api_)) == 0)
    {
        api_ = nullptr;
        FreeLibrary(module_);
        module_ = nullptr;
    }
}

RenderDocCapture::~RenderDocCapture()
{
    if (module_ != nullptr)
    {
        FreeLibrary(module_);
    }
}

void RenderDocCapture::SetCapturePath(const char* path)
{
    if (api_ != nullptr)
    {
        api_->SetCaptureFilePathTemplate(path);
    }
}

void RenderDocCapture::StartCapture()
{
    if (api_ != nullptr)
    {
        api_->StartFrameCapture(nullptr, nullptr);
    }
}

bool RenderDocCapture::EndCapture()
{
    return api_ != nullptr && api_->EndFrameCapture(nullptr, nullptr) == 1;
}

std::string RenderDocCapture::LastCapturePath() const
{
    if (api_ == nullptr)
    {
        return {};
    }

    const uint32_t count = api_->GetNumCaptures();
    if (count == 0)
    {
        return {};
    }

    char filename[512];
    uint32_t length = static_cast<uint32_t>(sizeof(filename));
    if (api_->GetCapture(count - 1, filename, &length, nullptr) == 0)
    {
        return {};
    }

    return std::string(filename, length);
}
} // namespace MmdLab
