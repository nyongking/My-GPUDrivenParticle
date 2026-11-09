#include "pch.h"
#include "TexturedQuad.h"

#include "GraphicsCore.h"
#include "GraphicsCommon.h"
#include "BufferManager.h"
#include "CommandContext.h"
#include "PipelineState.h"
#include "GpuBuffer.h"

#include "TextureConvert.h"

#include "CompiledShaders/TexturedQuadVS.h"
#include "CompiledShaders/TexturedQuadPS.h"

namespace
{
	struct  QuadVertex
	{
		XMFLOAT3	position;
		XMFLOAT2	uv;
	};

	enum RootParams
	{
		kViewProjCBV,
		kWorldCBV,
		kTintConstants,
		kTextureSRV,
		kNumRootParams
	};
	
	RootSignature		s_RootSig;
	GraphicsPSO			s_PSO(L"TexturedQuad PSO");
	ByteAddressBuffer	s_VertexBuffer;
	ByteAddressBuffer	s_IndexBuffer;
}

using namespace Graphics;
using namespace Math;

void QuadRenderer::Initialize()
{
	// RootSig
	s_RootSig.Reset(kNumRootParams, 1); // sampler : LinearSampler
	s_RootSig[kViewProjCBV].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX);
	s_RootSig[kWorldCBV].InitAsConstantBuffer(1, D3D12_SHADER_VISIBILITY_VERTEX);
	s_RootSig[kTintConstants].InitAsConstants(2, 4, D3D12_SHADER_VISIBILITY_PIXEL);
	s_RootSig[kTextureSRV].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
	s_RootSig.InitStaticSampler(0, SamplerLinearWrapDesc, D3D12_SHADER_VISIBILITY_PIXEL);
	s_RootSig.Finalize(L"TexturedQuad RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// InputLayout
	D3D12_INPUT_ELEMENT_DESC layout[] =
	{
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	// PSO
	s_PSO.SetRootSignature(s_RootSig);
	s_PSO.SetRasterizerState(RasterizerTwoSided);
	s_PSO.SetBlendState(BlendDisable);
	s_PSO.SetDepthStencilState(DepthStateReadWrite);
	s_PSO.SetInputLayout(_countof(layout), layout);
	s_PSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
	s_PSO.SetRenderTargetFormat(g_SceneColorBuffer.GetFormat(), g_SceneDepthBuffer.GetFormat());
	s_PSO.SetVertexShader(g_pTexturedQuadVS, sizeof(g_pTexturedQuadVS));
	s_PSO.SetPixelShader(g_pTexturedQuadPS, sizeof(g_pTexturedQuadPS));
	s_PSO.Finalize();

	// Create Quad VB, IB
	QuadVertex vertices[4] =
	{
		{XMFLOAT3(-0.5f, 1.f, 0.f), XMFLOAT2(0.f, 0.f)},
		{XMFLOAT3(0.5f, 1.f, 0.f), XMFLOAT2(1.f, 0.f)},
		{XMFLOAT3(0.5f, 0.f, 0.f), XMFLOAT2(1.f, 1.f)},
		{XMFLOAT3(-0.5f, 0.f, 0.f), XMFLOAT2(0.f, 1.f)}	
	};

	s_VertexBuffer.Create(L"Quad VB", 4, sizeof(QuadVertex), vertices);

	uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };

	s_IndexBuffer.Create(L"Quad IB", 6, sizeof(uint16_t), indices);
}

void QuadRenderer::Shutdown()
{
	s_VertexBuffer.Destroy();
	s_IndexBuffer.Destroy();

}

void QuadRenderer::BeginDraw(GraphicsContext& context, const Math::Matrix4& viewProj)
{
	context.SetRootSignature(s_RootSig);
	context.SetPipelineState(s_PSO);
	context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	context.SetVertexBuffer(0, s_VertexBuffer.VertexBufferView());
	context.SetIndexBuffer(s_IndexBuffer.IndexBufferView());

	context.SetDynamicConstantBufferView(kViewProjCBV, sizeof(viewProj), &viewProj);

	// 이후 각자의 TexturedQuad가 World matrix, Texture, Tint 'Bind'
}

bool TexturedQuad::Load(const std::wstring& texturePath, bool sRGB)
{
	// 먼저 각 Texture 파일 포맷을 dds로 변경
	CompileTextureOnDemand(texturePath, TextureOptions(sRGB, true, false));

	std::wstring ddsPath = Utility::RemoveExtension(texturePath) + L".dds";
	m_Texture = TextureManager::LoadDDSFromFile(ddsPath, Graphics::kMagenta2D, sRGB);

	if (!m_Texture.IsValid())
	{
		Utility::Printf(L"[TextureQuad] Texture load failed : %ws\n", ddsPath.c_str());
		m_Aspect = 1.f;
	}
	else
	{
		m_Aspect = (float)m_Texture->GetWidth() / (float)m_Texture->GetHeight();
	}
	
	UpdateWorldMatrix();

	return m_Texture.IsValid();
}

void TexturedQuad::Release()
{
	m_Texture = nullptr;

}

void TexturedQuad::SetTransform(const Math::Vector3& position, float rotationY, float height)
{
	m_Position = position;
	m_RotationY = rotationY;
	m_Height = height;

	UpdateWorldMatrix();
}

void TexturedQuad::Draw(GraphicsContext& context) const
{
	// world 
	context.SetDynamicConstantBufferView(kWorldCBV, sizeof(m_World), &m_World);

	// tint
	context.SetConstants(kTintConstants, m_Tint.R(), m_Tint.G(), m_Tint.B(), m_Tint.A());

	context.SetDynamicDescriptor(kTextureSRV, 0, m_Texture.GetSRV());

	context.DrawIndexed(6);
}

void TexturedQuad::UpdateWorldMatrix()
{
	Matrix3 base = Matrix3::MakeYRotation(m_RotationY) * Matrix3::MakeScale(m_Height * m_Aspect, m_Height, 1.f);
	m_World = Matrix4(base, m_Position);
}
