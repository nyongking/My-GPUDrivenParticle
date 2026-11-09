#pragma once
#include "VectorMath.h"
#include "Texture.h"
#include "TextureManager.h"

#include <string> // for loading texture path

class GraphicsContext;

namespace QuadRenderer
{
	void Initialize();
	void Shutdown();

	void BeginDraw(GraphicsContext& context, const Math::Matrix4& viewProj);
}

class TexturedQuad
{
public:
	bool Load(const std::wstring& texturePath, bool sRGB = true);
	void Release();
	void SetTransform(const Math::Vector3& position, float rotationY = 0.f, float height = 1.f);
	void SetTint(const Color& tint) { m_Tint = tint; }

	void Draw(GraphicsContext& context) const;
	

private:
	void UpdateWorldMatrix();

private:
	TextureRef	m_Texture;
	float		m_Aspect = 1.0f;
	Math::Vector3 m_Position = Math::Vector3(Math::kZero);
	float		m_RotationY = 0.f;
	float		m_Height = 1.f;
	Math::Matrix4 m_World = Math::Matrix4(Math::kIdentity);
	Color		m_Tint = Color(1.f, 1.f, 1.f);
};