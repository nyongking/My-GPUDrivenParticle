#pragma once
#include "AttributeType.h"
#include "TextureManager.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct MaterialGraph;

enum class TextureColorSpace : uint32_t
{
	sRGB,
	Linear,
	Count
};

// 슬롯 하나의 설정
struct MaterialTexture
{
	std::string         path; // 빈 문자열 = 비어 있음, 기본 텍스처 설정
	TextureColorSpace   colorSpace = TextureColorSpace::sRGB;

	bool operator==(const MaterialTexture& o) const { return path == o.path && colorSpace == o.colorSpace; }
	bool operator!=(const MaterialTexture& o) const { return !(*this == o); }
};

const char* TextureColorSpaceName(TextureColorSpace colorSpace);

enum class MaterialOp : uint32_t
{
	QuadUV,
	BaseColor,
	Coverage,
	Particle,
	Const,
	UVTransform,
	Sample,
	Add,
	Sub,
	Mul,
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
const char* MaterialOpName(MaterialOp op);

namespace MaterialSwizzle
{
	constexpr uint32_t XYZW = 0xE4; // 0b11100100
	constexpr uint32_t X = 0x00;	// 0b00000000
	constexpr uint32_t Y = 0x55;	// 0b01010101
	constexpr uint32_t Z = 0xAA;	// 0b10101010
	constexpr uint32_t W = 0xFF;	// 0b11111111
}

struct MaterialInstr
{
	uint32_t op, dst, srcA, srcB, srcC, swz, param, _pad;
};
static_assert(sizeof(MaterialInstr) == 32, "MaterialInstr 32 B");

struct MaterialProgram
{
	static const uint32_t kMaxInstrs = 256;
	static const uint32_t kMaxRegs = 16;

	std::vector<MaterialInstr>	instrs;
	uint32_t					outputReg = 0;
};

std::string DumpMaterialProgram(const MaterialProgram& program);

class ParticleMaterial
{
public:
	enum TextureSlot : uint32_t
	{
		kBaseTexture,
		kTexture0,
		kTexture1,
		kTexture2,
		kTexture3,
		kNumTextureSlot,
	};

	enum class Effect : uint32_t
	{
		None,
		Pattern,
		Tint,
		Dissolve,
		Distortion,
		Glow,
		Count
	};

	struct LayerLayout
	{
		Effect effect = Effect::None;
		uint32_t paramBase = 0; // 
		uint32_t paramCount = 0; // 
		uint32_t textureBase = 0; //
		uint32_t textureCount = 0; // 
	};

	static const uint32_t kNumEffectTextures = kNumTextureSlot - kTexture0;
	static const uint32_t kMaxParams = 16;
	static const uint32_t kNumCommonParams = 2;
	static const uint32_t kMaxLayers = 4;

	static const char*		SlotName(uint32_t slot);
	static MaterialTexture	DefaultTexture();
	
	static const char*	ParamName(uint32_t k, Effect effect = Effect::Pattern);
	static float4		DefaultParam(uint32_t k, Effect effect = Effect::Pattern);

	static const char*	EffectName(Effect effect);
	static uint32_t		NumParams(Effect effect);

	// 머티리얼을 그리는 두 가지 방식
	//   Preset : 효과 하나 (Effect). 효과마다 고정된 픽셀 셰이더 파일이 있다 (Shaders/SpritePreset*PS.hlsl)
	//   Custom : 그래프. MaterialShaderGenerator 가 그래프마다 픽셀 셰이더를 만든다
	// 효과를 둘 이상 섞으면 Preset 으로는 표현할 수 없으므로 Custom (섞은 결과의 그래프) 으로 넘어간다
	enum class Mode : uint32_t { Preset, Custom };

	// 단순 Effect Preset에 따른 Material 설계
	static MaterialProgram	BuildEffectProgram(Effect effect);
	static uint32_t		NumEffectTextures(Effect effect);
	static const char*	EffectTextureName(Effect effect, uint32_t local);
	static bool			LayoutLayers(const std::vector<Effect>& layers, std::vector<LayerLayout>* out, std::string* error);


public:
	ParticleMaterial();
	~ParticleMaterial();

	ParticleMaterial(const ParticleMaterial&) = delete;
	ParticleMaterial& operator=(const ParticleMaterial&) = delete;

	// ---------- Texture ----------
	bool						SetTexture(uint32_t slot, const MaterialTexture& texture);
	const MaterialTexture&		SlotTexture(uint32_t slot) const;

	D3D12_CPU_DESCRIPTOR_HANDLE TextureSRV(uint32_t slot) const;

	void						ReleaseTextures();

	// ---------- Parameters ----------
	void						SetParam(uint32_t k, const float4& value);
	const float4&				Param(uint32_t k) const;
	const float4*				Params() const { return m_Params; }
	void						ResetParams();

	// ---------- Effect ---------
	void						SetEffect(Effect effect);
	Effect						GetEffect() const { return m_Effect; }

	const std::vector<Effect>& Layers() const { return m_Layers; }
	const std::vector<LayerLayout>& LayerLayouts() const { return m_LayerLayouts; }
	bool						AddLayer(Effect effect);
	bool						RemoveLayer(uint32_t index);
	bool						MoveLayer(uint32_t index, int direction);	// -1 = 위로, +1 = 아래로
	bool						SetLayerEffect(uint32_t index, Effect effect);
	bool						SetLayers(const std::vector<Effect>& layers);
	const std::string& LayerError() const { return m_LayerError; }
	bool						NeedsVM() const { return m_UseVM || m_Layers.size() > 1 || m_CustomGraph != nullptr; }

	Mode						GetMode() const { return m_CustomGraph ? Mode::Custom : Mode::Preset; }
	// 지금의 효과 층을 그대로 그래프로 옮기고 Custom 으로 넘어간다 (층 목록은 비운다, 값 / 텍스처는 유지)
	void						ConvertLayersToCustomGraph();

	bool						HasCustomGraph() const { return m_CustomGraph != nullptr; }
	const MaterialGraph*		CustomGraph() const { return m_CustomGraph.get(); }
	void						SetCustomGraph(const MaterialGraph& graph);
	void						SetCustomGraphLayout(const MaterialGraph& graph);
	void						ClearCustomGraph();
	MaterialGraph				CurrentGraph() const;
	MaterialGraph				LayersAsGraph() const;

	void						SetUseVM(bool use) { m_UseVM = use; }
	bool						UseVM() const { return m_UseVM; }
	const MaterialProgram&		Program() const { return m_Program; }

	enum class ProgramSource : uint32_t { HandWritten, Graph, Count };
	void						SetProgramSource(ProgramSource source);
	ProgramSource				GetProgramSource() const { return m_ProgramSource; }
	const std::string&			ProgramError() const { return m_ProgramError; }

	// 그래프 / 레이어 구성이 바뀔 때마다 올라간다 (Generated 방식이 다시 만들 시점을 아는 용도)
	uint32_t					GraphRevision() const { return m_GraphRevision; }

private:
	bool						ApplyLayers(const std::vector<Effect>& layers, const std::vector<int>& from, bool keepTextures);
	void						RebuildProgram();
	void						SyncEffect();

	MaterialTexture		m_Textures[kNumTextureSlot];
	TextureRef			m_Refs[kNumTextureSlot];
	alignas(16) float4				m_Params[kMaxParams];
	Effect				m_Effect = Effect::None;
	std::vector<Effect>			m_Layers;
	std::vector<LayerLayout>	m_LayerLayouts;
	std::string					m_LayerError;
	bool				m_UseVM = false;
	MaterialProgram		m_Program;
	ProgramSource		m_ProgramSource = ProgramSource::HandWritten;
	std::string			m_ProgramError;
	uint32_t			m_GraphRevision = 0;
	std::unique_ptr<MaterialGraph> m_CustomGraph;

};

namespace ParticleTextures
{
	
	TextureRef Load(const std::string& path, TextureColorSpace colorSpace);

	// 원본 경로 -> 색공간별 변환 파일 경로 (sRGB = "X.dds" 기존 그대로, Linear = "X.linear.dds")
	std::wstring CachePath(const std::wstring& sourcePath, TextureColorSpace colorSpace);
	bool         IsCacheFile(const std::string& fileName);

	void Retire(TextureRef& ref);
	void CollectRetired();
	void Shutdown();
}
