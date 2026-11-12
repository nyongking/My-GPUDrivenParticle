#pragma once
#include "EmitterGpuProgram.h"
#include "PipelineState.h"

enum class GpuSimMode { Generated ,Uber};

class UberGpuProgram
{
public:
	static constexpr uint32_t kNumPSOs = EmitterGpuProgram::kNumKernels;
	
	static bool Initialize();
	static bool Ready();

	static const ComputePSO& PSO(EmitterGpuProgram::Kernel k);
	static void SetLayoutVerified(bool ok);
	static const char* StatusText();

	static GpuSimMode Mode();
	static void SetMode(GpuSimMode mode);
	static const char* ModeName(GpuSimMode mode);
};

