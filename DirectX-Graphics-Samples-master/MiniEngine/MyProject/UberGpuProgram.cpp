#include "pch.h"
#include "UberGpuProgram.h"

#include "CompiledShaders/ParticleUberEmitterUpdateCS.h"
#include "CompiledShaders/ParticleUberParticleUpdateCS.h"
#include "CompiledShaders/ParticleUberParticleSpawnCS.h"

namespace 
{
	ComputePSO s_PSO[UberGpuProgram::kNumPSOs] =
	{
		ComputePSO(L"Uber Emitter Update PSO"),
		ComputePSO(L"Uber Particle Update PSO"),
		ComputePSO(L"Uber Particle  PSO")

	};

	bool s_PSOReady = false;
	bool s_LayoutVerified = false;
	GpuSimMode s_Mode = GpuSimMode::Generated;
}

bool UberGpuProgram::Initialize()
{
	if (s_PSOReady)
		return true;

	s_PSO[0].SetRootSignature(EmitterGpuProgram::RootSig());
	s_PSO[0].SetComputeShader(g_pParticleUberEmitterUpdateCS, sizeof(g_pParticleUberEmitterUpdateCS));
	s_PSO[0].Finalize();

	s_PSO[1].SetRootSignature(EmitterGpuProgram::RootSig());
	s_PSO[1].SetComputeShader(g_pParticleUberParticleUpdateCS, sizeof(g_pParticleUberParticleUpdateCS));
	s_PSO[1].Finalize();

	s_PSO[2].SetRootSignature(EmitterGpuProgram::RootSig());
	s_PSO[2].SetComputeShader(g_pParticleUberParticleSpawnCS, sizeof(g_pParticleUberParticleSpawnCS));
	s_PSO[2].Finalize();

	s_PSOReady = true;
	Utility::Print("[UberGpuProgram] 공유 PSO 3개 생성\n");

	return true;
}

bool UberGpuProgram::Ready()
{
	return s_PSOReady && s_LayoutVerified;
}

const ComputePSO& UberGpuProgram::PSO(EmitterGpuProgram::Kernel k)
{
	return s_PSO[k];
}

void UberGpuProgram::SetLayoutVerified(bool ok)
{
	s_LayoutVerified = ok;
}

const char* UberGpuProgram::StatusText()
{
	if (!s_PSOReady)       
		return "PSOs not created yet";
	if (!s_LayoutVerified)
		return "cbuffer layout mismatch - Uber disabled (see Output window)";
	return "ready";
}

GpuSimMode UberGpuProgram::Mode()
{
	return s_Mode;
}

void UberGpuProgram::SetMode(GpuSimMode mode)
{
	s_Mode = mode;
}

const char* UberGpuProgram::ModeName(GpuSimMode mode)
{
	return mode == GpuSimMode::Uber ? "Uber" : "Generated";
}
