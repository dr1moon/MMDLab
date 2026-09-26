#pragma once

#include "Runtime/Asset/ImageLoader.h"
#include "Runtime/Asset/MeshAsset.h"
#include "Runtime/Core/FrameResource.h"
#include "Runtime/DX12/Dx12CommandQueue.h"
#include "Runtime/DX12/Dx12Device.h"
#include "Runtime/DX12/Dx12RootSignature.h"
#include "Runtime/DX12/Dx12SwapChain.h"
#include "Runtime/DX12/Shaders.h"

#include <wrl/client.h>

#include <d3d12.h>

#include <cstdint>
#include <span>
#include <vector>

struct ImDrawData;

namespace MmdLab
{
// The synchronous D3D12 renderer for a static mesh and an imgui overlay: a swap chain, a root
// signature carrying a camera constant buffer, per-material shading constants, and a
// three-texture table, an MMD toon mesh pipeline, uploaded vertex/index buffers, and a
// dedicated imgui SRV descriptor heap. Device-level state is created in the constructor; the
// model-specific buffers, textures, and constants are (re)built by SetModel(), which the
// RhiThread calls when the selected model's generation changes.
class Dx12Renderer final
{
public:
    Dx12Renderer(
        HWND window,
        std::uint32_t width,
        std::uint32_t height);

    Dx12Renderer(const Dx12Renderer&) = delete;
    Dx12Renderer& operator=(const Dx12Renderer&) = delete;

    // Uploads a model's CPU mesh and textures into GPU resources, first waiting for all
    // in-flight GPU work to complete so the previous model's resources can be released.
    void SetModel(const MeshAsset& mesh, std::span<const Image> textures);

    // Orbit/zoom the perspective camera. The RhiThread drives these from imgui input
    // (left-drag orbits, wheel zooms).
    void Orbit(float deltaX, float deltaY);
    void Zoom(float wheelDelta);

    // Records and submits one frame's draw list plus the imgui overlay, presents, and returns
    // the monotonic GPU fence value for this frame. The caller gates frame-resource reuse on it.
    [[nodiscard]] std::uint64_t Render(std::span<const DrawPacket> drawPackets, ImDrawData* uiDrawData);

    // True once the GPU has completed all work submitted through the frame with this fence value.
    [[nodiscard]] bool IsFrameComplete(std::uint64_t fenceValue) const;

    // Handles the RhiThread passes to imgui's DX12 backend.
    [[nodiscard]] ID3D12Device* GetDevice() const { return device_.Get(); }
    [[nodiscard]] ID3D12CommandQueue* GetQueue() const { return queue_.Get(); }
    [[nodiscard]] ID3D12DescriptorHeap* GetImGuiSrvHeap() const { return imguiSrvHeap_.Get(); }
    void AllocImGuiSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
    void FreeImGuiSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);

private:
    static constexpr std::uint32_t kFrameCount = 2;
    static constexpr std::uint32_t kImGuiSrvCount = 64;

    void WaitForPreviousFrame(std::uint32_t frameIndex);
    void WaitForGpuIdle();
    void CreatePipelineState();
    void CreateRenderTargetViews();
    void CreateDepthBuffer();
    void CreateImGuiSrvHeap();
    void ReleaseModelResources();
    void CreateMeshBuffers(const MeshAsset& mesh);
    void CreateTextures(std::span<const Image> images);
    void CreateConstantBuffer();
    void UpdateCameraConstants(std::uint32_t frameIndex);

    Dx12Device device_;
    Dx12CommandQueue queue_;
    Dx12SwapChain swapChain_;
    Dx12RootSignature rootSignature_;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineStateCulled_;       // CullMode = BACK.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineStateDoubleSided_;  // CullMode = NONE.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> backBuffers_[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocators_[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fences_[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12Fence> frameFence_; // Signals each submitted frame's completion.

    // imgui DX12 backend resources (device-level, shared across models).
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> imguiSrvHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE imguiSrvHeapCpuStart_{};
    D3D12_GPU_DESCRIPTOR_HANDLE imguiSrvHeapGpuStart_{};
    std::uint32_t imguiSrvDescriptorSize_ = 0;
    std::vector<std::uint32_t> imguiSrvFreeIndices_; // Free-list of imgui SRV descriptor slots.

    // Model-specific resources, rebuilt by SetModel().
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView_{};
    D3D12_INDEX_BUFFER_VIEW indexBufferView_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> constantBuffers_[kFrameCount];
    void* constantBufferMapped_[kFrameCount] = { nullptr, nullptr };

    std::span<const Material> materials_; // Immutable material array, owned by the mesh asset.
    std::vector<MaterialShaderParams> materialParams_; // CPU-side shading params, one per material.

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_; // Shader-visible CBV/SRV/UAV heap.
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> textures_; // GPU textures, one per image.
    Microsoft::WRL::ComPtr<ID3D12Resource> whiteTexture_;          // 1x1 white fallback.
    Microsoft::WRL::ComPtr<ID3D12Resource> toonRampTexture_;       // 256x1 default toon ramp fallback.
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> materialSrvBundles_; // 3-SRV bundle (base/toon/sphere) per material.
    std::uint32_t srvDescriptorSize_ = 0;
    bool modelSet_ = false;

    // Perspective orbit camera state, re-framed on each model load.
    float cameraTarget_[3] = { 0.0f, 0.0f, 0.0f };
    float cameraDistance_ = 1.0f;
    float cameraYaw_ = 0.0f;
    float cameraPitch_ = 0.0f;
    float cameraExtent_ = 1.0f;

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t rtvDescriptorSize_ = 0;
    std::uint64_t fenceValues_[kFrameCount] = {};
    std::uint64_t currentFence_ = 0;
    std::uint64_t frameFenceValue_ = 0;
};
} // namespace MmdLab
