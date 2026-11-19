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

const char* TextureColorSpaceName(TextureColorSpace colorSpace);

// 슬롯 하나의 설정 (에셋에 저장하는 값)
struct MaterialTexture
{
	std::string         path; // 빈 문자열 = 비어 있음, 기본 텍스처 설정
	TextureColorSpace   colorSpace = TextureColorSpace::sRGB;

	bool operator==(const MaterialTexture& o) const { return path == o.path && colorSpace == o.colorSpace; }
	bool operator!=(const MaterialTexture& o) const { return !(*this == o); }
};

class ParticleMaterial
{
public:
	enum TextureSlot : uint32_t
	{
		kBaseTexture,
		kLayerTexture,
		kNumTextureSlot,
	};
	static const char*		SlotName(uint32_t slot);
	static MaterialTexture	DefaultTexture();
	
public:
	ParticleMaterial();
	~ParticleMaterial();

	ParticleMaterial(const ParticleMaterial&) = delete;
	ParticleMaterial& operator=(const ParticleMaterial&) = delete;

	bool SetTexture(uint32_t slot, const MaterialTexture& texture);
	const MaterialTexture& SlotTexture(uint32_t slot) const;

	D3D12_CPU_DESCRIPTOR_HANDLE TextureSRV(uint32_t slot) const;

	void ReleaseTextures();

private:
	MaterialTexture		m_Textures[kNumTextureSlot];
	TextureRef			m_Refs[kNumTextureSlot];

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
