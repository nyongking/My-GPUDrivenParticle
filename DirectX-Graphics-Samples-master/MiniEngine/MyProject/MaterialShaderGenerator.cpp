#include "pch.h"
#include "MaterialShaderGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
	const char kComp[4] = { 'x', 'y', 'z', 'w' };

	const char* TypeName(uint32_t width)
	{
		switch (width)
		{
		case 1:  return "float";
		case 2:  return "float2";
		case 3:  return "float3";
		default: return "float4";
		}
	}

	std::string Var(uint32_t index)
	{
		return "v" + std::to_string(index);
	}

	std::string FloatText(float v)
	{
		if (!std::isfinite(v))
			v = 0.f;

		// 같은 float 로 다시 읽히는 가장 짧은 표기 (0.1f 를 0.100000001f 로 쓰지 않는다)
		char buf[40];
		for (int digits = 1; digits <= 9; ++digits)
		{
			snprintf(buf, sizeof(buf), "%.*g", digits, v);
			if (strtof(buf, nullptr) == v)
				break;
		}

		std::string s = buf;
		if (s.find_first_of(".e") == std::string::npos)
			s += ".0";
		return s + "f";
	}

	// 빈 핀 기본값: 필요한 성분 수만큼만 쓴다 (1 -> "1.0f", 2 -> "float2(0.0f, 0.0f)")
	std::string LiteralText(const float4& v, uint32_t width)
	{
		const float c[4] = { v.x, v.y, v.z, v.w };
		if (width <= 1)
			return FloatText(c[0]);

		std::string s = std::string(TypeName(width)) + "(";
		for (uint32_t k = 0; k < width; ++k)
			s += (k ? ", " : "") + FloatText(c[k]);
		return s + ")";
	}

	std::string Float2Text(float x, float y)
	{
		return "float2(" + FloatText(x) + ", " + FloatText(y) + ")";
	}

	// 입력 하나를 읽는 식
	struct Operand
	{
		std::string text;
		uint32_t    width = 4;		// text 의 실제 성분 수
		bool        outOfRange = false;	// 원본에 없는 성분을 읽음 (0 으로 채움)
	};

	// src 변수(성분 srcWidth 개)에서 swz 의 앞 width 개 성분을 읽는다
	//   v3 (float4), .xyzw, 2  ->  v3.xy
	//   v3 (float4), .xxxx, 1  ->  v3.x
	//   v3 (float),  아무거나  ->  v3      (숫자 하나는 그대로. 넓히는 것은 HLSL 또는 Widen 이 한다)
	Operand Read(uint32_t src, uint32_t srcWidth, uint32_t swz, uint32_t width)
	{
		Operand o;
		const std::string var = Var(src);

		if (srcWidth <= 1)
		{
			o.text = var;
			o.width = 1;
			return o;
		}

		width = (std::max)(1u, (std::min)(width, 4u));
		o.width = width;

		uint32_t comp[4];
		bool identity = (width == srcWidth);
		bool inRange = true;
		for (uint32_t k = 0; k < width; ++k)
		{
			comp[k] = (swz >> (2 * k)) & 3u;
			identity = identity && (comp[k] == k);
			inRange = inRange && (comp[k] < srcWidth);
		}

		if (inRange)
		{
			o.text = var;
			if (!identity)
			{
				o.text += '.';
				for (uint32_t k = 0; k < width; ++k)
					o.text += kComp[comp[k]];
			}
			return o;
		}

		// 원본에 없는 성분: VM 에서도 의미 없는 값이므로 0 으로 둔다
		o.outOfRange = true;
		if (width == 1)
		{
			o.text = "(0.f)";
			return o;
		}
		o.text = std::string(TypeName(width)) + "(";
		for (uint32_t k = 0; k < width; ++k)
		{
			o.text += (k ? ", " : "");
			o.text += (comp[k] < srcWidth) ? (var + "." + kComp[comp[k]]) : std::string("0.f");
		}
		o.text += ")";
		return o;
	}

	// 내장 함수(lerp / smoothstep / pow)에 넣을 때: 숫자 하나를 결과 너비로 넓힌다
	std::string Widen(const Operand& o, uint32_t width)
	{
		if (o.width == width || width <= 1)
			return o.text;
		return std::string("(") + TypeName(width) + ")" + o.text;
	}

	// UV 자리: float2 로 맞춘다
	std::string AsUV(const Operand& o)
	{
		if (o.width == 2) return o.text;
		if (o.width == 1) return o.text + ".xx";
		return o.text + ".xy";
	}
}

GeneratedShader GenerateMaterialShader(const MaterialGraph& graph, const std::string& fileName)
{
	const MaterialCompileResult compiled = CompileMaterialGraph(graph);
	if (!compiled.ssaOk)
	{
		GeneratedShader out;
		out.fileName = fileName;
		out.errors.push_back("Graph compile failed: " + compiled.error);
		return out;
	}
	return GenerateMaterialShader(compiled, fileName);
}

GeneratedShader GenerateMaterialShader(const MaterialCompileResult& compiled, const std::string& fileName)
{
	GeneratedShader out;
	out.fileName = fileName;

	if (!compiled.ssaOk)
	{
		out.errors.push_back("ssa 가 없음 (그래프 컴파일 실패)");
		return out;
	}

	const size_t count = compiled.ssa.size();
	if (compiled.ssaInfo.size() != count || compiled.ssaInputs.size() != count || compiled.ssaValues.size() != count * 2)
	{
		out.errors.push_back("ssa 타입 정보의 길이가 다름");
		return out;
	}

	std::string body;

	for (size_t k = 0; k < count; ++k)
	{
		const MaterialInstr& I = compiled.ssa[k];
		const MaterialSsaInfo& info = compiled.ssaInfo[k];
		const MaterialOp op = static_cast<MaterialOp>(I.op);
		const uint32_t inputs = (std::min)(compiled.ssaInputs[k], 3u);
		const uint32_t width = (std::max)(1u, (std::min)(uint32_t(info.width), 4u));	// 결과 타입
		const std::string who = "명령어 " + std::to_string(k) + " (" + MaterialOpName(op) + ")";

		// ---- 입력: 앞에서 만든 변수를 그 변수의 타입에 맞게 읽는다 ----
		const uint32_t src[3] = { I.srcA, I.srcB, I.srcC };
		Operand in[3];
		for (uint32_t j = 0; j < inputs; ++j)
		{
			if (src[j] >= k)
			{
				out.errors.push_back(who + " 가 아직 없는 값 v" + std::to_string(src[j]) + " 를 읽음");
				return out;
			}
			in[j] = Read(src[j], compiled.ssaInfo[src[j]].width, I.swz >> (8 * j), info.inWidth[j]);
			if (in[j].outOfRange)
				out.warnings.push_back(who + " 입력 " + std::string(1, static_cast<char>('a' + j)) + ": v" + std::to_string(src[j])
					+ " (" + TypeName(compiled.ssaInfo[src[j]].width) + ") 에 없는 성분을 읽어 0 으로 채움");
		}
		const Operand& a = in[0];
		const Operand& b = in[1];
		const Operand& c = in[2];

		const int autoIndex = (k < compiled.ssaAuto.size()) ? compiled.ssaAuto[k] : -1;
		const bool isAuto = autoIndex >= 0 && static_cast<size_t>(autoIndex) < compiled.autoValues.size();

		// ---- 노드 값 (숫자로 직접 쓴다) / 텍스처 슬롯 범위 ----
		const float4& value0 = compiled.ssaValues[2 * k];
		const float4& value1 = compiled.ssaValues[2 * k + 1];
		if (op == MaterialOp::Sample && I.param >= ParticleMaterial::kNumEffectTextures)
		{
			out.errors.push_back(who + ": 텍스처 슬롯 " + std::to_string(I.param) + " 는 없음");
			return out;
		}

		// ---- 식: MaterialVM.hlsli 의 case 와 같은 계산을, 필요한 성분만 ----
		std::string expr;
		uint32_t natural = width;	// 식이 원래 내는 성분 수 (결과 타입과 다르면 오류)
		switch (op)
		{
		case MaterialOp::QuadUV:    expr = "i.quadUV";         natural = 2; break;
		case MaterialOp::BaseColor: expr = "i.baseColor";      natural = 4; break;
		case MaterialOp::Coverage:  expr = "i.coverage";       natural = 1; break;
		case MaterialOp::Particle:  expr = "i.particleParams"; natural = 2; break;
		case MaterialOp::Const:
			// 연결하지 않은 핀의 값 / Constant 노드의 값: 둘 다 결과 타입 그대로의 숫자
			//   예) const float v3 = 3.0f;   const float4 v7 = float4(1.0f, 0.5f, 0.0f, 1.0f);
			expr = LiteralText(isAuto ? compiled.autoValues[autoIndex] : value0, width);
			break;
		case MaterialOp::UVTransform:
			// uv * tiling + offset + scroll * Time
			expr = AsUV(a) + " * " + Float2Text(value0.x, value0.y) + " + " + Float2Text(value0.z, value0.w) + " + "
				+ Float2Text(value1.x, value1.y) + " * Time";
			natural = 2;
			break;
		case MaterialOp::Sample:
			expr = "MaterialTextures[" + std::to_string(I.param) + "].Sample(LinearWrap, " + AsUV(a) + ")";
			natural = 4;
			break;
		case MaterialOp::BaseSample:
			expr = "SampleBaseOffset(i, " + AsUV(a) + ")";
			natural = 4;
			break;

		// 숫자 하나와 벡터가 섞이면 HLSL 이 넓혀 준다
		case MaterialOp::Add:          expr = a.text + " + " + b.text; break;
		case MaterialOp::Sub:          expr = a.text + " - " + b.text; break;
		case MaterialOp::Mul:          expr = a.text + " * " + b.text; break;
		case MaterialOp::OneMinus:     expr = "1.f - " + a.text; break;
		case MaterialOp::UnpackSigned: expr = a.text + " * 2.f - 1.f"; break;
		case MaterialOp::Saturate:     expr = "saturate(" + a.text + ")"; break;

		case MaterialOp::Lerp:
			expr = "lerp(" + Widen(a, width) + ", " + Widen(b, width) + ", " + Widen(c, width) + ")";
			break;
		case MaterialOp::Smoothstep:
			expr = "smoothstep(" + Widen(a, width) + ", " + Widen(b, width) + ", " + Widen(c, width) + ")";
			break;
		case MaterialOp::Power:
			expr = "pow(max(" + Widen(a, width) + ", 1e-6f), " + Widen(b, width) + ")";
			break;

		case MaterialOp::Append:
		{
			// (색, 알파) -> float4. 색이 숫자 하나면 rgb 에 같은 값
			std::string rgb;
			if (a.width == 1)      rgb = a.text + ".xxx";
			else if (a.width == 3) rgb = a.text;
			else if (a.width == 4) rgb = a.text + ".xyz";
			else
			{
				out.errors.push_back(who + " 입력 a: 색 자리에 float2");
				return out;
			}
			expr = "float4(" + rgb + ", " + (b.width == 1 ? b.text : b.text + ".x") + ")";
			natural = 4;
			break;
		}
		default:
			out.errors.push_back(who + ": 모르는 op " + std::to_string(I.op));
			return out;
		}

		if (natural != width)
		{
			out.errors.push_back(who + ": 결과는 " + TypeName(natural) + " 인데 타입 정보는 " + TypeName(width));
			return out;
		}

		body += std::string("    const ") + TypeName(width) + " " + Var(static_cast<uint32_t>(k)) + " = " + expr + ";";
		body += std::string("    // ") + MaterialOpName(op) + "\n";
	}

	std::string result = "i.baseColor";
	if (compiled.ssaOutput >= 0)
	{
		if (static_cast<size_t>(compiled.ssaOutput) >= count)
		{
			out.errors.push_back("출력 v" + std::to_string(compiled.ssaOutput) + " 가 없음");
			return out;
		}
		if (compiled.ssaInfo[compiled.ssaOutput].width != 4)
		{
			out.errors.push_back(std::string("출력은 float4 여야 하는데 ") + TypeName(compiled.ssaInfo[compiled.ssaOutput].width));
			return out;
		}
		result = Var(static_cast<uint32_t>(compiled.ssaOutput));
	}

	std::string& s = out.source;
	// 주석은 ASCII 로만: 이 문자열은 UTF-8 로 DXC 에 넘어간다
	s += "// generated by MaterialShaderGenerator - do not edit\n";
	s += "#include \"SpriteMaterialCommon.hlsli\"\n\n";
	s += "float4 EvalMaterial(MaterialInput i)\n{\n";
	s += body;
	s += "    return " + result + ";\n}\n\n";
	s += "float4 main(PSInput input) : SV_TARGET\n{\n";
	s += "    const MaterialInput mi = MakeMaterialInput(input);\n";
	s += "    const float4 result = EvalMaterial(mi);\n\n";
	s += "    const float visible = (BlendMode == 1) ? max(result.r, max(result.g, result.b)) : result.a;\n";
	s += "    if (visible < 0.01f)\n";
	s += "        discard;\n";
	s += "    return result;\n}\n";

	return out;
}
