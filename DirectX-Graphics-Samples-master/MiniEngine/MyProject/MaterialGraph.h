#pragma once
#include "ParticleMaterial.h"

#include <cstdint>
#include <string>
#include <vector>

enum class MaterialNodeType : uint32_t
{
	QuadUV,
	BaseColor,
	Coverage,
	ParticleParam,
	Constant,
	UVTransform,
	TextureSample,
	Add,
	Subtract,
	Multiply,
	Lerp,
	Saturate,
	OneMinus,
	Smoothstep,
	Append,
	UnpackSigned,
	BaseSample,
	Count
};
const char* MaterialNodeTypeName(MaterialNodeType type);
uint32_t    MaterialNodeInputCount(MaterialNodeType type);

struct MaterialLink
{
	int      node = -1;
	uint32_t swizzle = MaterialSwizzle::XYZW;
};

struct MaterialNode
{
	MaterialNodeType          type = MaterialNodeType::QuadUV;
	std::vector<MaterialLink> inputs;
	float4                    values[2] = {};
	uint32_t                  textureSlot = 0;
};

struct MaterialGraph
{
	std::vector<MaterialNode> nodes;
	int                       output = -1;
};

struct MaterialCompileResult
{
	bool                ok = false;
	std::string         error;
	MaterialProgram     program;
	std::vector<float4> params;
};

MaterialCompileResult CompileMaterialGraph(const MaterialGraph& graph);

MaterialGraph BuildEffectGraph(ParticleMaterial::Effect effect);

std::string DumpMaterialGraph(const MaterialGraph& graph);
