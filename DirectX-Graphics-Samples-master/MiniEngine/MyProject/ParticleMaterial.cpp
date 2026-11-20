#include "pch.h"
#include "ParticleMaterial.h"

#include "GraphicsCore.h"
#include "GraphicsCommon.h"
#include "CommandListManager.h"
#include "TextureConvert.h"

#include <vector>


const char* TextureColorSpaceName(TextureColorSpace colorSpace)
{
	switch (colorSpace)
	{
	case TextureColorSpace::sRGB:	return "sRGB";
	case TextureColorSpace::Linear: return "Linear";
	default:						return "?";
	}
}

const char* ParticleMaterial::SlotName(uint32_t slot)
{
	switch (slot)
	{
	case kBaseTexture: return "Base";
	case kTexture0: return "Tex0";
	case kTexture1: return "Tex1";
	case kTexture2: return "Tex2";
	case kTexture3: return "Tex3";
	default:			return "?";
	}
}

MaterialTexture ParticleMaterial::DefaultTexture()
{
	MaterialTexture t;
	t.colorSpace = TextureColorSpace::Linear;
	return t;
}

const char* ParticleMaterial::ParamName(uint32_t k, Effect effect)
{
	if (effect == Effect::None)
		return nullptr;

	if (k == 0) return "Tex0 Tiling(xy) Offset(zw)";
	if (k == 1) return "Tex0 Scroll(xy)";

	switch (effect)
	{
	case Effect::Dissolve:
		if (k == 2) return "Soft(x) EdgeWidth(y)";
		if (k == 3) return "Edge Color(rgb)";
		break;
	case Effect::Distortion:
		if (k == 2) return "Strength(xy)";
		break;
	case Effect::Glow:
		if (k == 2) return "Tex1 Tiling(xy) Offset(zw)";
		if (k == 3) return "Tex1 Scroll(xy)";
		if (k == 4) return "Glow Color(rgb)";
		break;
	default:
		break;
	}
	return nullptr;
}

float4 ParticleMaterial::DefaultParam(uint32_t k, Effect effect)
{
	if (k == 0) return float4{ 1.f, 1.f, 0.f, 0.f };
	if (k == 1) return float4{ 0.f, 0.f, 0.f, 0.f };

	switch (effect)
	{
	case Effect::Dissolve:
		if (k == 2) return float4{ 0.05f, 0.05f, 0.f, 0.f };   // 부드러움, 테두리 폭, 진행도
		if (k == 3) return float4{ 1.f, 1.f, 1.f, 0.f };      // 테두리 색 (1 이상 가능)
		break;
	case Effect::Distortion:
		if (k == 2) return float4{ 0.05f, 0.05f, 0.f, 0.f };      // 사각형 UV 단위
		break;
	case Effect::Glow:
		if (k == 2) return float4{ 1.f, 1.f, 0.f, 0.f };        // Tex1 타일링 / 오프셋
		if (k == 3) return float4{ 0.f, 0.f, 0.f, 0.f };        // Tex1 스크롤
		if (k == 4) return float4{ 1.f, 1.f, 1.f, 0.f };      // 발광 색
		break;
	default:
		break;
	}
	return float4{ 0.f, 0.f, 0.f, 0.f };
}

const char* ParticleMaterial::EffectName(Effect effect)
{
	switch (effect)
	{
	case Effect::None: return "None";
	case Effect::Pattern: return "Pattern";
	case Effect::Tint: return "Tint";
	case Effect::Dissolve: return "Dissolve";
	case Effect::Distortion: return "Distortion";
	case Effect::Glow: return "Glow";
	default: return nullptr;
	}
}

uint32_t ParticleMaterial::NumParams(Effect effect)
{
	switch (effect)
	{
	case Effect::None: return 0;
	case Effect::Pattern: return 2;
	case Effect::Tint: return 2;
	case Effect::Dissolve: return 4;
	case Effect::Distortion: return 3;
	case Effect::Glow: return 5;
	default: return 0;
	}
}

ParticleMaterial::ParticleMaterial()
{
	for (uint32_t i = 0; i < kNumTextureSlot; ++i)
		m_Textures[i] = DefaultTexture();
	ResetParams();
}

ParticleMaterial::~ParticleMaterial()
{
	ReleaseTextures();
}

bool ParticleMaterial::SetTexture(uint32_t slot, const MaterialTexture& texture)
{
	if (slot >= kNumTextureSlot)
		return false;
	
	if (texture == m_Textures[slot])
		return texture.path.empty() || m_Refs[slot].IsValid();

	ParticleTextures::Retire(m_Refs[slot]);
	m_Textures[slot] = texture;

	if (texture.path.empty())
		return true;

	TextureRef loaded = ParticleTextures::Load(texture.path, texture.colorSpace);
	m_Refs[slot] = loaded;
	return m_Refs[slot].IsValid();
}

const MaterialTexture& ParticleMaterial::SlotTexture(uint32_t slot) const
{
	ASSERT(slot < kNumTextureSlot);
	return m_Textures[slot];
}

D3D12_CPU_DESCRIPTOR_HANDLE ParticleMaterial::TextureSRV(uint32_t slot) const
{
	if (slot >= kNumTextureSlot || m_Textures[slot].path.empty())
		return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
	return m_Refs[slot].GetSRV();	
}

void ParticleMaterial::ReleaseTextures()
{
	for (uint32_t i = 0; i < kNumTextureSlot; ++i)
	{
		ParticleTextures::Retire(m_Refs[i]);
		m_Textures[i] = DefaultTexture();
	}
}

void ParticleMaterial::SetParam(uint32_t k, const float4& value)
{
	if (k < kMaxParams)
		m_Params[k] = value;
}

const float4& ParticleMaterial::Param(uint32_t k) const
{
	ASSERT(k < kMaxParams);
	return m_Params[k];
}

void ParticleMaterial::ResetParams()
{
	for (uint32_t i = 0; i < kMaxParams; ++i)
		m_Params[i] = DefaultParam(i, m_Effect);
}

void ParticleMaterial::SetEffect(Effect effect)
{
	if (effect == m_Effect)
		return;
	m_Effect = effect;

	for (uint32_t k = kNumCommonParams; k < kMaxParams; ++k) 
		m_Params[k] = DefaultParam(k, effect);
}



// ----------------- Particle Textures Namespace -----------------

namespace
{
	struct RetiredTexture
	{
		TextureRef ref;
		uint64_t   fence = 0;
	};
	std::vector<RetiredTexture> s_Retired;

	// suffix로 끝나는가?
	bool EndsWith(const std::wstring& s, const std::wstring& suffix)
	{
		return s.size() >= suffix.size() 
			&& s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	bool IsSRGBFormat(DXGI_FORMAT format)
	{
		switch (format)
		{
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_BC1_UNORM_SRGB:
		case DXGI_FORMAT_BC2_UNORM_SRGB:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		case DXGI_FORMAT_BC7_UNORM_SRGB:
			return true;
		default:
			return false;
		}
	}

};

namespace ParticleTextures
{
	TextureRef Load(const std::string& path, TextureColorSpace colorSpace)
	{
		const std::wstring wpath = Utility::UTF8ToWideString(path);
		const bool srgb = (colorSpace == TextureColorSpace::sRGB);
		const bool isDDS = Utility::ToLower(Utility::GetFileExtension(wpath)) == L"dds";

		std::wstring ddsPath = wpath;
		if (!isDDS)
		{
			ddsPath = CachePath(wpath, colorSpace);
			uint32_t flags = TextureOptions(srgb, true);
			if (!srgb)
				flags |= kIgnoreSRGBMetaData;
			CompileTextureOnDemand(wpath, ddsPath, flags);
		}

		TextureRef ref = TextureManager::LoadDDSFromFile(ddsPath, Graphics::kMagenta2D, srgb);
		if (!ref.IsValid())
		{
			Utility::Printf(L"[ParticleTextures] Texture load failed : %ws\n", wpath.c_str());
			return ref;
		}

		if (!srgb)
		{
			ID3D12Resource* resource = const_cast<ID3D12Resource*>(ref->GetResource());
			if (resource != nullptr && IsSRGBFormat(resource->GetDesc().Format))
				Utility::Printf(L"[ParticleTextures] '%ws' is stored as sRGB -> read as sRGB, not Linear. Pick the source image (png) instead.\n", wpath.c_str());
		}
		return ref;
	}

	std::wstring CachePath(const std::wstring& sourcePath, TextureColorSpace colorSpace)
	{
		const std::wstring stem = Utility::RemoveExtension(sourcePath);
		return (colorSpace == TextureColorSpace::Linear) ? stem + L".linear.dds" : stem + L".dds";
	}

	bool IsCacheFile(const std::string& fileName)
	{
		std::wstring lower = Utility::ToLower(Utility::UTF8ToWideString(fileName));
		return EndsWith(lower, L".linear.dds");
	}

	void Retire(TextureRef& ref)
	{
		if (ref.Get() != nullptr)
		{
			RetiredTexture r;
			r.ref = ref;
			s_Retired.push_back(r);
		}
		ref = nullptr;
	}

	void CollectRetired()
	{
		if (s_Retired.empty())
			return;

		CommandQueue& queue = Graphics::g_CommandManager.GetGraphicsQueue();

		const uint64_t last = queue.GetNextFenceValue() - 1;

		std::vector<RetiredTexture> keeps;
		for (RetiredTexture& r : s_Retired)
		{
			if (r.fence == 0) // fence가 정해지지 않는 경우
				r.fence = last;

			if (!queue.IsFenceComplete(r.fence))
				keeps.push_back(r);
		}
		s_Retired.swap(keeps);
	}

	void Shutdown()
	{
		if (!s_Retired.empty())
			Graphics::g_CommandManager.IdleGPU();
		s_Retired.clear();
	}
}