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
	int      node = -1;		// -1일 경우 기본값을 넣는다.
	uint32_t swizzle = MaterialSwizzle::XYZW;
	uint32_t width = 0;		// 0 = 노드 출력 그대로, 1~4 = 해당 값만큼 데이터 쓰기
};

struct MaterialNode
{
	MaterialNodeType          type = MaterialNodeType::QuadUV;
	std::vector<MaterialLink> inputs;
	float4                    values[2] = {};
	uint32_t                  textureSlot = 0;
	float2						pos = { 0.f, 0.f };
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
	uint32_t                 graphParamCount = 0;
	std::vector<uint32_t>    nodeWidth;
	std::vector<std::string> notes;
	uint32_t			regCount = 0; // 실제로 사용한 Reg 칸 수
};

MaterialCompileResult CompileMaterialGraph(const MaterialGraph& graph);

MaterialGraph BuildEffectGraph(ParticleMaterial::Effect effect);
MaterialGraph BuildLayeredEffectGraph(const std::vector<ParticleMaterial::Effect>& layers);

std::string DumpMaterialGraph(const MaterialGraph& graph, const MaterialCompileResult* compiled = nullptr);

