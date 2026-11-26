#pragma once

#include "MaterialGraph.h"

#include <cstdint>

namespace MaterialGraphEditor
{
	enum Change : uint32_t
	{
		kNoChange = 0,
		kLayout = 1 << 0,
		kGraph = 1 << 1,
		// 값을 드래그하는 중 (아직 놓지 않음). 값을 놓으면 kGraph 가 온다
		//   Generated 방식은 값이 셰이더에 숫자로 들어가므로, 드래그하는 동안에는 셰이더를 다시 만들지 않는다
		kValue = 1 << 2,
	};

	uint32_t Draw(const char* editorId, MaterialGraph& graph, const MaterialCompileResult& compiled, bool editable);

	void Shutdown();
}
