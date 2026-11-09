#pragma once
#include "VectorMath.h"
#include "Color.h"

/// <summary>
/// 디버그용 선 그리기 모듈
/// 매 프레임 Add@로 선을 축적하고, RenderScene에서 Render 호출하면 그린 뒤 목록을 비운다.
/// </summary>

class GraphicsContext;

namespace DebugDraw
{
	void Initialize();
	void Shutdown();

	void AddLine(const Math::Vector3& a, const Math::Vector3& b, const Color& color);

	void Render(GraphicsContext& context, const Math::Matrix4& viewProj);
}
