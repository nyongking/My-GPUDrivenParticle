#pragma once
#include "AttributeType.h"
#include "TextureManager.h"

#include <cstdint>
#include <string>

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

	enum ParamSlot : uint32_t
	{
		kParamLayerUV,
		kParamLayerScroll,
		kNumNamedParams
	};

	enum class Effect : uint32_t
	{
		Pattern,
		Tint,
		Dissolve,
		Distortion,
		Glow,
		Count
	};

	static const uint32_t kNumEffectTextures = kNumTextureSlot - kTexture0;
	static const uint32_t kMaxParams = 16;
	static const uint32_t kNumCommonParams = 2;

	static const char*		SlotName(uint32_t slot);
	static MaterialTexture	DefaultTexture();
	
	static const char*	ParamName(uint32_t k, Effect effect = Effect::Pattern);
	static float4		DefaultParam(uint32_t k, Effect effect = Effect::Pattern);

	static const char*	EffectName(Effect effect);
	static uint32_t		NumParams(Effect effect);



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

private:
	MaterialTexture		m_Textures[kNumTextureSlot];
	TextureRef			m_Refs[kNumTextureSlot];
	alignas(16) float4				m_Params[kMaxParams];
	Effect				m_Effect = Effect::Pattern;

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
