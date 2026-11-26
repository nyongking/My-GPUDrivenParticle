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
	Power,
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

// 명령어 하나의 타입 정보 (Generated 방식만 쓴다. VM 은 전부 float4 로 계산)
//   width 1 ~ 4 = float / float2 / float3 / float4
struct MaterialSsaInfo
{
	uint8_t width = 4;					// 결과의 성분 수
	uint8_t inWidth[3] = { 4, 4, 4 };	// 입력마다 읽는 성분 수 (그 입력 swizzle 의 앞 N 개)
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

	// ---- Generated 방식(MaterialShaderGenerator)용: 칸 배정 전의 명령어 목록 ----
	//   ssa[k].dst == k, srcA/B/C = 앞선 명령어 번호. 값 하나 = 변수 하나라 Reg 8 칸 제한이 없다
	//   ok 가 false 여도 ssaOk 가 true 일 수 있다 (VM 의 Reg 칸 수만 넘은 경우)
	bool                       ssaOk = false;
	std::vector<MaterialInstr> ssa;
	std::vector<uint32_t>      ssaInputs;	// 명령어가 실제로 읽는 입력 수 (0 ~ 3)
	std::vector<MaterialSsaInfo> ssaInfo;	// ssa 와 같은 길이: 결과 / 입력의 성분 수
	std::vector<int>           ssaAuto;		// >= 0 이면 빈 핀 기본값 (autoValues 번호). 셰이더에 숫자로 직접 쓴다
	std::vector<float4>        autoValues;
	int                        ssaOutput = -1;	// 출력 명령어 번호. -1 = 출력 없음 (base color)
};

MaterialCompileResult CompileMaterialGraph(const MaterialGraph& graph);

// 하나의 Effect인 경우의 MaterialGraph 생성
MaterialGraph BuildEffectGraph(ParticleMaterial::Effect effect);
// 여러 Effect로 합쳐진 경우의 MaterialGraph 생성
MaterialGraph BuildLayeredEffectGraph(const std::vector<ParticleMaterial::Effect>& layers);

std::string DumpMaterialGraph(const MaterialGraph& graph, const MaterialCompileResult* compiled = nullptr);

const char* MaterialNodePinName(MaterialNodeType type, uint32_t pin);
bool        MaterialNodeTypeFromName(const std::string& name, MaterialNodeType& out);
void        LayoutMaterialGraph(MaterialGraph& graph);
bool        HasMaterialGraphLayout(const MaterialGraph& graph);

MaterialGraph KeepMaterialGraphNodes(const MaterialGraph& graph, const std::vector<bool>& keep);
MaterialGraph PruneMaterialGraph(const MaterialGraph& graph);
bool          FillMaterialGraphValues(MaterialGraph& graph, const float4* params, uint32_t count);

class JsonValue;
std::string MaterialSwizzleText(uint32_t swizzle);
bool        MaterialSwizzleFromText(const std::string& text, uint32_t& out);
JsonValue   MaterialGraphToJson(const MaterialGraph& graph);
bool        MaterialGraphFromJson(const JsonValue& json, MaterialGraph& out, std::vector<std::string>& warnings);

