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
	};

	uint32_t Draw(const char* editorId, MaterialGraph& graph, const MaterialCompileResult& compiled, bool editable);

	void Shutdown();
}
