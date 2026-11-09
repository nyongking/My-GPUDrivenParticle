//
// Dear ImGui integration for MiniEngine (Win32 + DX12 backends).
//
// ImGui is drawn into g_OverlayBuffer (R8G8B8A8_UNORM) as the last step of the
// UI pass, so it is composited on top of the scene and EngineTuning.
//

#pragma once

#include "imgui.h"

class GraphicsContext;

namespace ImGuiLayer
{
    void Initialize( void );
    void Shutdown( void );

    // Call once per frame before any ImGui:: widget calls.
    void NewFrame( void );

    // Records ImGui draw commands. Must be the last thing recorded on this context
    // because the ImGui backend binds its own root signature and PSO directly.
    void Render( GraphicsContext& Context );

    // Copies an SRV into the ImGui descriptor heap so it can be shown with ImGui::Image().
    ImTextureID RegisterTexture( D3D12_CPU_DESCRIPTOR_HANDLE SrvHandle );
    void UnregisterTexture( ImTextureID TexId );

    // Camera mode: DirectInput owns the mouse (exclusive, hidden cursor) and ImGui ignores it.
    // UI mode: cursor is visible and the mouse drives ImGui only. Toggled with F1.
    void SetCameraMode( bool Enable );
    bool IsCameraMode( void );
}
