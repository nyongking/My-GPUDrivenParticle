#include "pch.h"
#include "DebugDraw.h"

#include "GraphicsCore.h"
#include "GraphicsCommon.h"
#include "BufferManager.h"
#include "CommandContext.h"
#include "RootSignature.h"
#include "PipelineState.h"

#include "CompiledShaders/DebugLineVS.h"
#include "CompiledShaders/DebugLinePS.h"

#include <vector>
#include "TexturedQuad.h"

using namespace Graphics;
using namespace Math;

namespace
{
	struct alignas(16) LineVertex
	{
		XMFLOAT3	position;
		float		pad;
		XMFLOAT4	color;
	};
	static_assert(sizeof(LineVertex) == 32, "Must be 32 Bytes");

	enum RootParams { kViewProjCBV, kNumRootParams };

	RootSignature		s_RootSig;
	GraphicsPSO			s_LinePSO(L"DebugDraw: Line PSO");
	std::vector<LineVertex> s_Vertices;
}

void DebugDraw::Initialize()
{
	// --------RootSig--------
	s_RootSig.Reset(kNumRootParams, 0);
	s_RootSig[kViewProjCBV].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX); // b0
	s_RootSig.Finalize(L"DebugDraw RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT); // RootSignatureFlag 설정이후 마무리
	
	// InputLayout
	D3D12_INPUT_ELEMENT_DESC layout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
	};

	// --------PSO------------
	s_LinePSO.SetRootSignature(s_RootSig);
	s_LinePSO.SetRasterizerState(RasterizerTwoSided);
	s_LinePSO.SetBlendState(BlendDisable);
	s_LinePSO.SetDepthStencilState(DepthStateReadWrite);
	s_LinePSO.SetInputLayout(_countof(layout), layout);
	s_LinePSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
	s_LinePSO.SetRenderTargetFormat(g_SceneColorBuffer.GetFormat(), g_SceneDepthBuffer.GetFormat());
	s_LinePSO.SetVertexShader(g_pDebugLineVS, sizeof(g_pDebugLineVS));
	s_LinePSO.SetPixelShader(g_pDebugLinePS, sizeof(g_pDebugLinePS));
	s_LinePSO.Finalize();

	s_Vertices.reserve(4096);
}

void DebugDraw::Shutdown()
{
	s_Vertices.clear();
	s_Vertices.shrink_to_fit();
}

void DebugDraw::AddLine(const Vector3& a, const Vector3& b, const Color& color)
{
	LineVertex va;
	LineVertex vb;

	XMStoreFloat3(&va.position, a);
	XMStoreFloat3(&vb.position, b);
	XMStoreFloat4(&va.color, color);
	vb.color = va.color;

	s_Vertices.push_back(va);
	s_Vertices.push_back(vb);

}

void DebugDraw::Render(GraphicsContext& context, const Math::Matrix4& viewProj)
{
	if (s_Vertices.empty())
		return;

	// RootSignature, PSO, Topology bind
	context.SetRootSignature(s_RootSig);
	context.SetPipelineState(s_LinePSO);
	context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);

	// CBV
	context.SetDynamicConstantBufferView(kViewProjCBV, sizeof(viewProj), &viewProj);

	// VB
	context.SetDynamicVB(0, s_Vertices.size(), sizeof(LineVertex), s_Vertices.data());
	
	// draw
	context.Draw((UINT)s_Vertices.size());

	// @ Clear @
	s_Vertices.clear();
}
