#include "pch.h"
#include "ParticleMaterial.h"

#include "GraphicsCore.h"
#include "GraphicsCommon.h"
#include "CommandListManager.h"
#include "TextureConvert.h"
#include "MaterialGraph.h"

#include <vector>
#include <cstdio>


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

namespace
{
	uint32_t OpInputCount(MaterialOp op)
	{
		switch (op)
		{
		case MaterialOp::UVTransform: case MaterialOp::Sample: case MaterialOp::Saturate:
		case MaterialOp::OneMinus: case MaterialOp::UnpackSigned: case MaterialOp::BaseSample:
			return 1;
		case MaterialOp::Add: case MaterialOp::Sub: case MaterialOp::Mul: case MaterialOp::Append:
			return 2;
		case MaterialOp::Lerp: case MaterialOp::Smoothstep:
			return 3;
		default:
			return 0;
		}
	}

	const char* OpParamText(MaterialOp op)
	{
		switch (op)
		{
		case MaterialOp::Const:       return "param";
		case MaterialOp::UVTransform: return "param";
		case MaterialOp::Sample:      return "slot";
		default:                      return nullptr;
		}
	}

	// register, swizzle 내용을 문자열로 변경하는 함수
	std::string RegText(uint32_t reg, uint32_t swz)
	{
		std::string s = "r" + std::to_string(reg);
		if (swz == MaterialSwizzle::XYZW)
			return s;
		const char kComp[4] = { 'x', 'y', 'z', 'w' };
		const uint32_t c0 = swz & 3, c1 = (swz >> 2) & 3, c2 = (swz >> 4) & 3, c3 = (swz >> 6) & 3;
		s += '.';
		s += kComp[c0];
		if (!(c0 == c1 && c1 == c2 && c2 == c3))
		{
			s += kComp[c1];
			s += kComp[c2];
			s += kComp[c3];
		}
		return s;
	}

	struct Src
	{
		uint32_t reg = 0;
		uint32_t swz = MaterialSwizzle::XYZW;
	};
	Src R(uint32_t reg, uint32_t swz = MaterialSwizzle::XYZW) { Src s; s.reg = reg; s.swz = swz; return s; }

	struct ProgramWriter
	{
		MaterialProgram program;

		uint32_t Emit(MaterialOp op, Src a = Src(), Src b = Src(), Src c = Src(), uint32_t param = 0)
		{
			MaterialInstr I = {};
			I.op = static_cast<uint32_t>(op);
			I.dst = static_cast<uint32_t>(program.instrs.size());
			I.srcA = a.reg; I.srcB = b.reg; I.srcC = c.reg;
			I.swz = a.swz | (b.swz << 8) | (c.swz << 16);
			I.param = param;
			program.instrs.push_back(I);
			return I.dst;
		}

		MaterialProgram Finish(uint32_t output)
		{
			program.outputReg = output;
			ASSERT(program.instrs.size() <= MaterialProgram::kMaxInstrs);
			ASSERT(program.instrs.size() <= MaterialProgram::kMaxRegs);
			return program;
		}
	};
}

MaterialProgram ParticleMaterial::BuildEffectProgram(Effect effect)
{
	using namespace MaterialSwizzle;
	ProgramWriter w;

	switch (effect)
	{
	case Effect::Pattern:
	case Effect::Tint:
	{
		const uint32_t q = w.Emit(MaterialOp::QuadUV);
		const uint32_t uv = w.Emit(MaterialOp::UVTransform, R(q), Src(), Src(), 0);
		const uint32_t tex = w.Emit(MaterialOp::Sample, R(uv), Src(), Src(), 0);
		const uint32_t base = w.Emit(MaterialOp::BaseColor);
		const uint32_t mul = w.Emit(MaterialOp::Mul, R(base), R(tex, effect == Effect::Pattern ? X : XYZW));
		const uint32_t out = w.Emit(MaterialOp::Append, R(mul), R(base, W));
		return w.Finish(out);
	}
	case Effect::Dissolve:
	{
		const uint32_t q = w.Emit(MaterialOp::QuadUV);
		const uint32_t uv = w.Emit(MaterialOp::UVTransform, R(q), Src(), Src(), 0);
		const uint32_t mask = w.Emit(MaterialOp::Sample, R(uv), Src(), Src(), 0);
		const uint32_t age = w.Emit(MaterialOp::Particle);
		const uint32_t c2 = w.Emit(MaterialOp::Const, Src(), Src(), Src(), 2);
		const uint32_t n4 = w.Emit(MaterialOp::Add, R(age, X), R(c2, X));
		const uint32_t n5 = w.Emit(MaterialOp::Smoothstep, R(age, X), R(n4, X), R(mask, X));
		const uint32_t base = w.Emit(MaterialOp::BaseColor);
		const uint32_t n6 = w.Emit(MaterialOp::Mul, R(base), R(n5, X));
		const uint32_t n7 = w.Emit(MaterialOp::Add, R(n4, X), R(c2, Y));
		const uint32_t s = w.Emit(MaterialOp::Smoothstep, R(n4, X), R(n7, X), R(mask, X));
		const uint32_t n8 = w.Emit(MaterialOp::OneMinus, R(s));
		const uint32_t cov = w.Emit(MaterialOp::Coverage);
		const uint32_t t0 = w.Emit(MaterialOp::Mul, R(n8, X), R(n5, X));
		const uint32_t t1 = w.Emit(MaterialOp::Mul, R(t0, X), R(cov, X));
		const uint32_t c3 = w.Emit(MaterialOp::Const, Src(), Src(), Src(), 3);
		const uint32_t edge = w.Emit(MaterialOp::Mul, R(c3), R(t1, X));
		const uint32_t n9 = w.Emit(MaterialOp::Add, R(n6), R(edge));
		const uint32_t out = w.Emit(MaterialOp::Append, R(n9), R(n6, W));
		return w.Finish(out);
	}
	case Effect::Distortion:
	{
		const uint32_t q = w.Emit(MaterialOp::QuadUV);
		const uint32_t uv = w.Emit(MaterialOp::UVTransform, R(q), Src(), Src(), 0);
		const uint32_t n2 = w.Emit(MaterialOp::Sample, R(uv), Src(), Src(), 0);
		const uint32_t sn = w.Emit(MaterialOp::UnpackSigned, R(n2));
		const uint32_t c2 = w.Emit(MaterialOp::Const, Src(), Src(), Src(), 2);
		const uint32_t off = w.Emit(MaterialOp::Mul, R(sn), R(c2));
		const uint32_t base = w.Emit(MaterialOp::BaseSample, R(off));
		const uint32_t n3 = w.Emit(MaterialOp::Add, R(q), R(off));
		const uint32_t n4 = w.Emit(MaterialOp::Sample, R(n3), Src(), Src(), 1);
		const uint32_t mul = w.Emit(MaterialOp::Mul, R(base), R(n4, X));
		const uint32_t out = w.Emit(MaterialOp::Append, R(mul), R(base, W));
		return w.Finish(out);
	}
	case Effect::Glow:
	{
		const uint32_t q = w.Emit(MaterialOp::QuadUV);
		const uint32_t uv0 = w.Emit(MaterialOp::UVTransform, R(q), Src(), Src(), 0);
		const uint32_t n2 = w.Emit(MaterialOp::Sample, R(uv0), Src(), Src(), 0);
		const uint32_t uv1 = w.Emit(MaterialOp::UVTransform, R(q), Src(), Src(), 2);
		const uint32_t m2 = w.Emit(MaterialOp::Sample, R(uv1), Src(), Src(), 1);
		const uint32_t n3 = w.Emit(MaterialOp::Mul, R(n2, X), R(m2, X));
		const uint32_t cov = w.Emit(MaterialOp::Coverage);
		const uint32_t t0 = w.Emit(MaterialOp::Mul, R(n3, X), R(cov, X));
		const uint32_t c4 = w.Emit(MaterialOp::Const, Src(), Src(), Src(), 4);
		const uint32_t glow = w.Emit(MaterialOp::Mul, R(c4), R(t0, X));
		const uint32_t base = w.Emit(MaterialOp::BaseColor);
		const uint32_t n4 = w.Emit(MaterialOp::Add, R(base), R(glow));
		const uint32_t out = w.Emit(MaterialOp::Append, R(n4), R(base, W));
		return w.Finish(out);
	}
	default:
		return MaterialProgram();
	}
}

ParticleMaterial::ParticleMaterial()
{
	for (uint32_t i = 0; i < kNumTextureSlot; ++i)
		m_Textures[i] = DefaultTexture();
	ResetParams();
	RebuildProgram();
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

	RebuildProgram();
}

void ParticleMaterial::SetProgramSource(ProgramSource source)
{
	if (source == m_ProgramSource)
		return;
	m_ProgramSource = source;
	RebuildProgram();
}

void ParticleMaterial::RebuildProgram()
{
	m_ProgramError.clear();

	if (m_ProgramSource == ProgramSource::HandWritten)
	{
		m_Program = BuildEffectProgram(m_Effect);
		return;
	}

	const MaterialCompileResult result = CompileMaterialGraph(BuildEffectGraph(m_Effect));
	if (!result.ok)
	{
		m_ProgramError = "Graph compile failed: " + result.error;
		return;
	}
	m_Program = result.program;

	for (size_t k = result.graphParamCount; k < result.params.size(); ++k)
		m_Params[k] = result.params[k];

	for (size_t k = 0; k < result.graphParamCount; ++k)
	{
		const float4 d = DefaultParam(static_cast<uint32_t>(k), m_Effect);
		const float4& p = result.params[k];
		if (d.x != p.x || d.y != p.y || d.z != p.z || d.w != p.w)
		{
			m_ProgramError = "Graph param slot " + std::to_string(k) + " differs from DefaultParam (slot layout mismatch)";
			break;
		}
	}
}

const char* MaterialOpName(MaterialOp op)
{
	switch (op)
	{
	case MaterialOp::QuadUV:       return "QUAD_UV";
	case MaterialOp::BaseColor:    return "BASE_COLOR";
	case MaterialOp::Coverage:     return "COVERAGE";
	case MaterialOp::Particle:     return "PARTICLE";
	case MaterialOp::Const:        return "CONST";
	case MaterialOp::UVTransform:  return "UV_TRANSFORM";
	case MaterialOp::Sample:       return "SAMPLE";
	case MaterialOp::Add:          return "ADD";
	case MaterialOp::Sub:          return "SUB";
	case MaterialOp::Mul:          return "MUL";
	case MaterialOp::Lerp:         return "LERP";
	case MaterialOp::Saturate:     return "SATURATE";
	case MaterialOp::OneMinus:     return "ONE_MINUS";
	case MaterialOp::Smoothstep:   return "SMOOTHSTEP";
	case MaterialOp::Append:       return "APPEND";
	case MaterialOp::UnpackSigned: return "UNPACK_SIGNED";
	case MaterialOp::BaseSample:   return "BASE_SAMPLE";
	default:                       return "?";
	}
}



// Imgui에 연산 방식을 문자열로 출력하기 위한 함수
std::string DumpMaterialProgram(const MaterialProgram& program)
{
	if (program.instrs.empty())
		return "(no instructions = base color)\n";

	std::string out;
	char line[160];
	for (size_t k = 0; k < program.instrs.size(); ++k)
	{
		const MaterialInstr& I = program.instrs[k];
		const MaterialOp op = static_cast<MaterialOp>(I.op);

		std::string args;
		const uint32_t n = OpInputCount(op);
		const uint32_t srcs[3] = { I.srcA, I.srcB, I.srcC };
		for (uint32_t s = 0; s < n; ++s)
		{
			if (!args.empty()) 
				args += ", ";
			args += RegText(srcs[s], (I.swz >> (8 * s)) & 0xFF);
		}
		if (const char* pt = OpParamText(op))
		{
			if (!args.empty()) 
				args += ", ";
			args += std::string(pt) + " " + std::to_string(I.param);
		}

		snprintf(line, sizeof(line), "%2u  %-14s r%-2u <- %s\n", uint32_t(k), MaterialOpName(op), I.dst, args.c_str());
		out += line;
	}
	snprintf(line, sizeof(line), "output = r%u (%u instrs)\n", program.outputReg, uint32_t(program.instrs.size()));
	out += line;
	return out;
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