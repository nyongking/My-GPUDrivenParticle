//
// Dear ImGui integration for MiniEngine (Win32 + DX12 backends).
//

#include "pch.h"
#include "ImGuiLayer.h"
#include "GraphicsCore.h"
#include "CommandContext.h"
#include "CommandListManager.h"
#include "BufferManager.h"
#include "GameInput.h"

#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

#include <vector>

namespace GameCore { extern HWND g_hWnd; }

using namespace Graphics;
using Microsoft::WRL::ComPtr;

namespace
{
    // Font atlas + user textures registered through RegisterTexture().
    const uint32_t kSrvHeapSize = 64;

    // More than the 3 swap chain buffers so the backend never overwrites a vertex
    // buffer the GPU may still be reading (DXGI allows up to 3 queued frames).
    const int kNumFramesInFlight = 4;

    ComPtr<ID3D12DescriptorHeap> s_SrvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE s_SrvHeapCpuStart;
    D3D12_GPU_DESCRIPTOR_HANDLE s_SrvHeapGpuStart;
    uint32_t s_SrvDescriptorSize = 0;
    std::vector<uint32_t> s_FreeSrvSlots;

    bool s_Initialized = false;
    bool s_CameraMode = false;

    bool AllocSrv( D3D12_CPU_DESCRIPTOR_HANDLE* OutCpu, D3D12_GPU_DESCRIPTOR_HANDLE* OutGpu )
    {
        if (s_FreeSrvSlots.empty())
            return false;

        uint32_t Slot = s_FreeSrvSlots.back();
        s_FreeSrvSlots.pop_back();
        OutCpu->ptr = s_SrvHeapCpuStart.ptr + (SIZE_T)Slot * s_SrvDescriptorSize;
        OutGpu->ptr = s_SrvHeapGpuStart.ptr + (UINT64)Slot * s_SrvDescriptorSize;
        return true;
    }

    void FreeSrv( D3D12_GPU_DESCRIPTOR_HANDLE Gpu )
    {
        uint32_t Slot = (uint32_t)((Gpu.ptr - s_SrvHeapGpuStart.ptr) / s_SrvDescriptorSize);
        ASSERT(Slot < kSrvHeapSize);
        s_FreeSrvSlots.push_back(Slot);
    }

    void BackendAllocSrv( ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* OutCpu, D3D12_GPU_DESCRIPTOR_HANDLE* OutGpu )
    {
        bool Succeeded = AllocSrv(OutCpu, OutGpu);
        ASSERT(Succeeded, "ImGui SRV heap is full");
        (void)Succeeded;
    }

    void BackendFreeSrv( ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE Gpu )
    {
        FreeSrv(Gpu);
    }
}

void ImGuiLayer::Initialize( void )
{
    ASSERT(!s_Initialized);

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
    HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HeapDesc.NumDescriptors = kSrvHeapSize;
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ASSERT_SUCCEEDED(g_Device->CreateDescriptorHeap(&HeapDesc, MY_IID_PPV_ARGS(&s_SrvHeap)));
    s_SrvHeap->SetName(L"ImGui SRV Heap");

    s_SrvHeapCpuStart = s_SrvHeap->GetCPUDescriptorHandleForHeapStart();
    s_SrvHeapGpuStart = s_SrvHeap->GetGPUDescriptorHandleForHeapStart();
    s_SrvDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    s_FreeSrvSlots.clear();
    for (uint32_t i = kSrvHeapSize; i > 0; --i)
        s_FreeSrvSlots.push_back(i - 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(GameCore::g_hWnd);

    ImGui_ImplDX12_InitInfo InitInfo;
    InitInfo.Device = g_Device;
    InitInfo.CommandQueue = g_CommandManager.GetCommandQueue();
    InitInfo.NumFramesInFlight = kNumFramesInFlight;
    InitInfo.RTVFormat = g_OverlayBuffer.GetFormat();
    InitInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    InitInfo.SrvDescriptorHeap = s_SrvHeap.Get();
    InitInfo.SrvDescriptorAllocFn = BackendAllocSrv;
    InitInfo.SrvDescriptorFreeFn = BackendFreeSrv;
    ImGui_ImplDX12_Init(&InitInfo);

    s_Initialized = true;
    SetCameraMode(false);
}

void ImGuiLayer::Shutdown( void )
{
    if (!s_Initialized)
        return;

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    s_SrvHeap = nullptr;
    s_FreeSrvSlots.clear();
    s_Initialized = false;
}

void ImGuiLayer::NewFrame( void )
{
    if (GameInput::IsFirstPressed(GameInput::kKey_f1))
        SetCameraMode(!s_CameraMode);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // While ImGui is typing into a widget, keep keystrokes away from the game.
    GameInput::SetKeyboardSuppressed(!s_CameraMode && ImGui::GetIO().WantCaptureKeyboard);
}

void ImGuiLayer::Render( GraphicsContext& Context )
{
    ImGui::Render();

    Context.SetRenderTarget(g_OverlayBuffer.GetRTV());
    Context.SetViewportAndScissor(0, 0, g_OverlayBuffer.GetWidth(), g_OverlayBuffer.GetHeight());

    // Bind through the context so its descriptor heap cache matches what the GPU sees.
    Context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_SrvHeap.Get());
    Context.FlushResourceBarriers();

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), Context.GetCommandList());
}

ImTextureID ImGuiLayer::RegisterTexture( D3D12_CPU_DESCRIPTOR_HANDLE SrvHandle )
{
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu;
    if (!AllocSrv(&Cpu, &Gpu))
    {
        ASSERT(false, "ImGui SRV heap is full");
        return ImTextureID_Invalid;
    }

    g_Device->CopyDescriptorsSimple(1, Cpu, SrvHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return (ImTextureID)Gpu.ptr;
}

void ImGuiLayer::UnregisterTexture( ImTextureID TexId )
{
    if (TexId == ImTextureID_Invalid)
        return;

    D3D12_GPU_DESCRIPTOR_HANDLE Gpu;
    Gpu.ptr = (UINT64)TexId;
    FreeSrv(Gpu);
}

void ImGuiLayer::SetCameraMode( bool Enable )
{
    s_CameraMode = Enable;
    GameInput::SetMouseCaptured(Enable);

    ImGuiIO& io = ImGui::GetIO();
    if (Enable)
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    else
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
}

bool ImGuiLayer::IsCameraMode( void )
{
    return s_CameraMode;
}
