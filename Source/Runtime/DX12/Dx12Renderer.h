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
#include <unordered_map>
#include <vector>

struct ImDrawData;

namespace MmdLab
{
struct Camera;
struct Model;
struct ModelInstance;

// View-only debug overlays, toggled from the imgui panel and passed to Render each frame. They
// live on the RhiThread and never round-trip to the GameThread; the renderer just draws them.
struct DebugViewOptions
{
    bool showSkeleton = false;
    bool showSkinningColors = false;
};

// The synchronous D3D12 renderer for a composed level of static meshes plus an imgui overlay:
// a swap chain, a root signature carrying a camera constant buffer, per-material shading
// constants, a per-instance world matrix, and a three-texture table, an MMD toon mesh pipeline,
// and a dedicated imgui SRV descriptor heap. Device-level state is created in the constructor;
// each model's buffers, textures, and constants are built on demand by EnsureModelsResident(),
// which the RhiThread calls when the selected level's generation changes.
class Dx12Renderer final
{
public:
    Dx12Renderer(
        HWND window,
        std::uint32_t width,
        std::uint32_t height);

    Dx12Renderer(const Dx12Renderer&) = delete;
    Dx12Renderer& operator=(const Dx12Renderer&) = delete;
    ~Dx12Renderer();

    // Builds GPU resources for every model referenced by `instances` that is not yet resident.
    // Called once per level switch; already-resident models are reused.
    void EnsureModelsResident(
        std::span<const ModelInstance> instances,
        std::span<const Model> models);

    // Records and submits one frame's draw list plus the imgui overlay, presents, and returns
    // the monotonic GPU fence value for this frame. The caller gates frame-resource reuse on it.
    [[nodiscard]] std::uint64_t Render(
        std::span<const ModelInstance> instances,
        std::span<const Model> models,
        const Camera& camera,
        ImDrawData* uiDrawData,
        DebugViewOptions options);

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

    // GPU-side resources for one resident model, keyed in residentModels_ by model index.
    struct GpuModel
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
        D3D12_VERTEX_BUFFER_VIEW vertexView{};
        D3D12_INDEX_BUFFER_VIEW indexView{};
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> textures;
        Microsoft::WRL::ComPtr<ID3D12Resource> whiteTexture;
        Microsoft::WRL::ComPtr<ID3D12Resource> toonRampTexture;
        std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> materialSrvBundles;
        std::vector<MaterialShaderParams> materialParams;
        std::span<const Material> materials; // Immutable array, owned by the mesh asset.

        // Skeleton line overlay (interleaved position + color) and the skinning-color second
        // vertex stream (per-vertex flat color, parallel to the mesh positions).
        Microsoft::WRL::ComPtr<ID3D12Resource> skeletonVertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW skeletonVertexView{};
        std::uint32_t skeletonVertexCount = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> skinningColorBuffer;
        D3D12_VERTEX_BUFFER_VIEW skinningColorView{};
        std::uint32_t totalIndexCount = 0;
    };

    void WaitForPreviousFrame(std::uint32_t frameIndex);
    void WaitForGpuIdle();
    void CreatePipelineState();
    void CreateRenderTargetViews();
    void CreateDepthBuffer();
    void CreateImGuiSrvHeap();
    void BuildGpuModel(std::size_t modelIndex, const Model& model);
    void CreateMeshBuffers(GpuModel& model, const Model& cpuModel);
    void CreateTextures(GpuModel& model, std::span<const Image> images);
    void CreateConstantBuffer();
    void UpdateCameraConstants(std::uint32_t frameIndex, const Camera& camera);

    Dx12Device device_;
    Dx12CommandQueue queue_;
    Dx12SwapChain swapChain_;
    Dx12RootSignature rootSignature_;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineStateCulled_;       // CullMode = BACK.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineStateDoubleSided_;  // CullMode = NONE.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineStateLines_;        // Skeleton overlay (LINE list).
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineStateSkinning_;     // Skinning-color view (two streams).
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> backBuffers_[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocators_[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fences_[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12Fence> frameFence_; // Signals each submitted frame's completion.

    // One auto-reset event reused by every fence wait (WaitForPreviousFrame and WaitForGpuIdle),
    // created once instead of allocating a kernel event per wait. Single-owner: the RhiThread.
    HANDLE fenceEvent_ = nullptr;

    // imgui DX12 backend resources (device-level, shared across models).
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> imguiSrvHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE imguiSrvHeapCpuStart_{};
    D3D12_GPU_DESCRIPTOR_HANDLE imguiSrvHeapGpuStart_{};
    std::uint32_t imguiSrvDescriptorSize_ = 0;
    std::vector<std::uint32_t> imguiSrvFreeIndices_; // Free-list of imgui SRV descriptor slots.

    // Resident GPU models, keyed by model index. Shared across levels: a model appearing in
    // multiple levels is uploaded once.
    std::unordered_map<std::size_t, GpuModel> residentModels_;

    // Camera constant buffer, device-level and written per frame by UpdateCameraConstants().
    Microsoft::WRL::ComPtr<ID3D12Resource> constantBuffers_[kFrameCount];
    void* constantBufferMapped_[kFrameCount] = { nullptr, nullptr };
    std::uint32_t srvDescriptorSize_ = 0;

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t rtvDescriptorSize_ = 0;
    std::uint64_t fenceValues_[kFrameCount] = {};
    std::uint64_t currentFence_ = 0;
    std::uint64_t frameFenceValue_ = 0;
};
} // namespace MmdLab
