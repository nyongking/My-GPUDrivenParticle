#include "pch.h"
#include "MaterialGraph.h"

#include <cstdio>

const char* MaterialNodeTypeName(MaterialNodeType type)
{
	switch (type)
	{
	case MaterialNodeType::QuadUV:        return "QuadUV";
	case MaterialNodeType::BaseColor:     return "BaseColor";
	case MaterialNodeType::Coverage:      return "Coverage";
	case MaterialNodeType::ParticleParam: return "ParticleParam";
	case MaterialNodeType::Constant:      return "Constant";
	case MaterialNodeType::UVTransform:   return "UVTransform";
	case MaterialNodeType::TextureSample: return "TextureSample";
	case MaterialNodeType::Add:           return "Add";
	case MaterialNodeType::Subtract:      return "Subtract";
	case MaterialNodeType::Multiply:      return "Multiply";
	case MaterialNodeType::Lerp:          return "Lerp";
	case MaterialNodeType::Saturate:      return "Saturate";
	case MaterialNodeType::OneMinus:      return "OneMinus";
	case MaterialNodeType::Smoothstep:    return "Smoothstep";
	case MaterialNodeType::Append:        return "Append";
	case MaterialNodeType::UnpackSigned:  return "UnpackSigned";
	case MaterialNodeType::BaseSample:    return "BaseSample";
	default:                              return "?";
	}
}

uint32_t MaterialNodeInputCount(MaterialNodeType type)
{
	switch (type)
	{
	case MaterialNodeType::UVTransform: case MaterialNodeType::TextureSample: case MaterialNodeType::Saturate:
	case MaterialNodeType::OneMinus: case MaterialNodeType::UnpackSigned: case MaterialNodeType::BaseSample:
		return 1;
	case MaterialNodeType::Add: case MaterialNodeType::Subtract: case MaterialNodeType::Multiply: case MaterialNodeType::Append:
		return 2;
	case MaterialNodeType::Lerp: case MaterialNodeType::Smoothstep:
		return 3;
	default:
		return 0;
	}
}

namespace
{
	MaterialOp OpOf(MaterialNodeType type)
	{
		switch (type)
		{
		case MaterialNodeType::QuadUV:        return MaterialOp::QuadUV;
		case MaterialNodeType::BaseColor:     return MaterialOp::BaseColor;
		case MaterialNodeType::Coverage:      return MaterialOp::Coverage;
		case MaterialNodeType::ParticleParam: return MaterialOp::Particle;
		case MaterialNodeType::Constant:      return MaterialOp::Const;
		case MaterialNodeType::UVTransform:   return MaterialOp::UVTransform;
		case MaterialNodeType::TextureSample: return MaterialOp::Sample;
		case MaterialNodeType::Add:           return MaterialOp::Add;
		case MaterialNodeType::Subtract:      return MaterialOp::Sub;
		case MaterialNodeType::Multiply:      return MaterialOp::Mul;
		case MaterialNodeType::Lerp:          return MaterialOp::Lerp;
		case MaterialNodeType::Saturate:      return MaterialOp::Saturate;
		case MaterialNodeType::OneMinus:      return MaterialOp::OneMinus;
		case MaterialNodeType::Smoothstep:    return MaterialOp::Smoothstep;
		case MaterialNodeType::Append:        return MaterialOp::Append;
		case MaterialNodeType::UnpackSigned:  return MaterialOp::UnpackSigned;
		case MaterialNodeType::BaseSample:    return MaterialOp::BaseSample;
		default:                              return MaterialOp::Count;
		}
	}

	uint32_t ParamSlotsOf(MaterialNodeType type)
	{
		switch (type)
		{
		case MaterialNodeType::Constant:    return 1;
		case MaterialNodeType::UVTransform: return 2;
		default:                            return 0;
		}
	}

	constexpr int kNotYet = -1;
	constexpr int kVisiting = -2;

	const char* WidthName(uint32_t width)
	{
		switch (width)
		{
		case 1:  return "float";
		case 2:  return "float2";
		case 3:  return "float3";
		default: return "float4";
		}
	}

	bool IsSingleComponent(uint32_t swz)
	{
		swz &= 0xFFu;
		return swz == (swz & 3u) * 0x55u;
	}

	uint32_t Broadcast(uint32_t swz)
	{
		return (swz & 3u) * 0x55u;
	}

	char PinName(uint32_t pin)
	{
		return static_cast<char>('a' + pin);
	}

	struct PinDefault
	{
		bool        quadUV = false;
		float4      value = { 0.f, 0.f, 0.f, 0.f };
		uint32_t    width = 1;
		const char* text = "0";
	};

	PinDefault DefaultForPin(MaterialNodeType type, uint32_t pin)
	{
		PinDefault d;
		switch (type)
		{
		case MaterialNodeType::UVTransform:
		case MaterialNodeType::TextureSample:
			d.quadUV = true; d.width = 2; d.text = "QuadUV";
			break;
		case MaterialNodeType::BaseSample:
			d.width = 2; d.text = "(0, 0)";
			break;
		case MaterialNodeType::Multiply:
			if (pin == 1) { d.value = { 1.f, 1.f, 1.f, 1.f }; d.text = "1"; }
			break;
		case MaterialNodeType::Lerp:
			if (pin == 2) { d.value = { 0.5f, 0.5f, 0.5f, 0.5f }; d.text = "0.5"; }
			break;
		case MaterialNodeType::Smoothstep:
			if (pin == 1) { d.value = { 1.f, 1.f, 1.f, 1.f }; d.text = "1"; }
			break;
		case MaterialNodeType::Append:
			if (pin == 0) { d.width = 4; d.text = "(0, 0, 0, 0)"; }
			else          { d.value = { 1.f, 1.f, 1.f, 1.f }; d.text = "1"; }
			break;
		default:
			break;
		}
		return d;
	}

	struct InputInfo
	{
		uint32_t reg = 0;
		uint32_t swz = MaterialSwizzle::XYZW;
		uint32_t width = 4;
		bool     autoFilled = false;
	};

	class GraphCompiler
	{
	public:
		explicit GraphCompiler(const MaterialGraph& graph)
			: m_Graph(graph), m_RegOf(graph.nodes.size(), kNotYet), m_Width(graph.nodes.size(), 0)
		{
		}

		MaterialCompileResult Run()
		{
			MaterialCompileResult result;
			result.nodeWidth.assign(m_Graph.nodes.size(), 0);

			if (m_Graph.output < 0)
			{
				result.ok = true;
				return result;
			}

			const int outputReg = Compile(m_Graph.output);
			if (outputReg < 0)
				return Failed(result);

			if (m_Width[m_Graph.output] != 4)
			{
				Fail("출력 노드 n" + std::to_string(m_Graph.output) + " 는 float4 (색 + 알파) 여야 하는데 "
					+ WidthName(m_Width[m_Graph.output]) + ". 색과 알파를 Append 로 합쳐 주세요");
				return Failed(result);
			}

			std::vector<int> slotOf(m_Graph.nodes.size(), -1);
			for (size_t id = 0; id < m_Graph.nodes.size(); ++id)
			{
				if (m_RegOf[id] < 0)
					continue;
				const MaterialNode& n = m_Graph.nodes[id];
				const uint32_t count = ParamSlotsOf(n.type);
				if (count == 0)
					continue;
				if (!ReserveParams(result, count, "노드 n" + std::to_string(id)))
					return Failed(result);
				slotOf[id] = static_cast<int>(result.params.size());
				for (uint32_t k = 0; k < count; ++k)
					result.params.push_back(n.values[k]);
			}
			result.graphParamCount = static_cast<uint32_t>(result.params.size());

			std::vector<int> autoSlot(m_AutoValues.size(), -1);
			for (size_t a = 0; a < m_AutoValues.size(); ++a)
			{
				if (!ReserveParams(result, 1, "빈 핀 기본값"))
					return Failed(result);
				autoSlot[a] = static_cast<int>(result.params.size());
				result.params.push_back(m_AutoValues[a]);
			}

			for (size_t k = 0; k < m_Instrs.size(); ++k)
			{
				if (m_InstrAuto[k] >= 0)
				{
					m_Instrs[k].param = static_cast<uint32_t>(autoSlot[m_InstrAuto[k]]);
					continue;
				}
				if (m_InstrNode[k] < 0)
					continue;
				const MaterialNode& n = m_Graph.nodes[m_InstrNode[k]];
				if (ParamSlotsOf(n.type) > 0)
					m_Instrs[k].param = static_cast<uint32_t>(slotOf[m_InstrNode[k]]);
				else if (n.type == MaterialNodeType::TextureSample)
					m_Instrs[k].param = n.textureSlot;
			}

			result.ok = true;
			result.program.instrs = m_Instrs;
			result.program.outputReg = static_cast<uint32_t>(outputReg);
			result.notes = m_Notes;
			for (size_t id = 0; id < m_Graph.nodes.size(); ++id)
				result.nodeWidth[id] = (m_RegOf[id] >= 0) ? m_Width[id] : 0u;
			return result;
		}

	private:
		int Fail(const std::string& message)
		{
			if (m_Error.empty())
				m_Error = message;
			return -1;
		}

		MaterialCompileResult& Failed(MaterialCompileResult& result)
		{
			result.ok = false;
			result.error = m_Error;
			result.program = MaterialProgram();
			result.params.clear();
			result.graphParamCount = 0;
			result.notes.clear();
			result.nodeWidth.assign(m_Graph.nodes.size(), 0);
			return result;
		}

		bool ReserveParams(const MaterialCompileResult& result, uint32_t count, const std::string& who)
		{
			if (result.params.size() + count <= ParticleMaterial::kMaxParams)
				return true;
			Fail("값 칸이 " + std::to_string(ParticleMaterial::kMaxParams) + " 개를 넘음 (" + who + ")");
			return false;
		}

		std::string NodeText(int id) const
		{
			return "n" + std::to_string(id) + " (" + MaterialNodeTypeName(m_Graph.nodes[id].type) + ")";
		}

		int Emit(MaterialInstr I, int node, int autoIndex)
		{
			if (m_Instrs.size() >= MaterialProgram::kMaxInstrs)
				return Fail("명령어가 " + std::to_string(MaterialProgram::kMaxInstrs) + " 개를 넘음");
			if (m_Instrs.size() >= MaterialProgram::kMaxRegs)
				return Fail("칸이 " + std::to_string(MaterialProgram::kMaxRegs) + " 개를 넘음 (칸 재사용은 아직 없음)");
			I.dst = static_cast<uint32_t>(m_Instrs.size());
			m_Instrs.push_back(I);
			m_InstrNode.push_back(node);
			m_InstrAuto.push_back(autoIndex);
			return static_cast<int>(I.dst);
		}

		bool DefaultInput(int id, uint32_t pin, InputInfo& out)
		{
			const PinDefault d = DefaultForPin(m_Graph.nodes[id].type, pin);
			int reg = -1;
			if (d.quadUV)
			{
				if (m_AutoQuadUV < 0)
				{
					MaterialInstr I = {};
					I.op = static_cast<uint32_t>(MaterialOp::QuadUV);
					I.swz = 0xE4E4E4u;
					m_AutoQuadUV = Emit(I, -1, -1);
				}
				reg = m_AutoQuadUV;
			}
			else
			{
				MaterialInstr I = {};
				I.op = static_cast<uint32_t>(MaterialOp::Const);
				I.swz = 0xE4E4E4u;
				m_AutoValues.push_back(d.value);
				reg = Emit(I, -1, static_cast<int>(m_AutoValues.size()) - 1);
			}
			if (reg < 0)
				return false;

			out.reg = static_cast<uint32_t>(reg);
			out.swz = MaterialSwizzle::XYZW;
			out.width = d.width;
			out.autoFilled = true;
			m_Notes.push_back(NodeText(id) + " 입력 " + PinName(pin) + " 가 비어 있어 기본값 " + d.text + " 사용");
			return true;
		}

		bool CheckUV(int id, uint32_t pin, const InputInfo& in)
		{
			if (in.width <= 2)
				return true;
			Fail(NodeText(id) + " 입력 " + PinName(pin) + ": UV 자리에 " + WidthName(in.width)
				+ " 를 연결함. 앞 두 성분(xy)만 쓰려면 연결의 width 를 2 로");
			return false;
		}

		uint32_t CheckTypes(int id, const InputInfo* in, uint32_t count)
		{
			const MaterialNodeType type = m_Graph.nodes[id].type;
			switch (type)
			{
			case MaterialNodeType::QuadUV:        return 2;
			case MaterialNodeType::ParticleParam: return 2;
			case MaterialNodeType::Coverage:      return 1;
			case MaterialNodeType::BaseColor:     return 4;
			case MaterialNodeType::Constant:      return 4;
			case MaterialNodeType::UVTransform:   return CheckUV(id, 0, in[0]) ? 2u : 0u;
			case MaterialNodeType::TextureSample: return CheckUV(id, 0, in[0]) ? 4u : 0u;
			case MaterialNodeType::BaseSample:    return CheckUV(id, 0, in[0]) ? 4u : 0u;
			case MaterialNodeType::Append:
				if (in[0].width == 2)
				{
					Fail(NodeText(id) + " 입력 a: 색(float3 / float4) 자리에 float2 를 연결함");
					return 0;
				}
				if (in[1].width != 1)
				{
					Fail(NodeText(id) + " 입력 b: 알파 자리에는 숫자 하나(float). " + WidthName(in[1].width)
						+ " 를 연결함. .x / .w 처럼 성분을 하나 고르세요");
					return 0;
				}
				return 4;
			default:
			{
				uint32_t width = 1;
				for (uint32_t k = 0; k < count; ++k)
				{
					if (in[k].width == 1)
						continue;
					if (width > 1 && in[k].width != width)
					{
						std::string list;
						for (uint32_t j = 0; j < count; ++j)
							list += std::string(j ? ", " : "") + PinName(j) + " = " + WidthName(in[j].width);
						Fail(NodeText(id) + ": 입력 타입이 다름 (" + list + "). 숫자 하나가 아니면 너비가 같아야 합니다");
						return 0;
					}
					width = in[k].width;
				}
				return width;
			}
			}
		}

		int Compile(int id)
		{
			if (!m_Error.empty())
				return -1;
			if (id < 0 || id >= static_cast<int>(m_Graph.nodes.size()))
				return Fail("없는 노드 n" + std::to_string(id) + " 를 연결함");

			if (m_RegOf[id] >= 0)
				return m_RegOf[id];
			if (m_RegOf[id] == kVisiting)
				return Fail("순환 연결 (노드 n" + std::to_string(id) + " 가 자기 결과를 다시 입력으로 받음)");
			m_RegOf[id] = kVisiting;

			const MaterialNode& n = m_Graph.nodes[id];
			const uint32_t need = MaterialNodeInputCount(n.type);
			if (n.inputs.size() > need)
				return Fail(NodeText(id) + ": 입력은 " + std::to_string(need) + " 개까지인데 " + std::to_string(n.inputs.size()) + " 개");
			if (n.type == MaterialNodeType::TextureSample && n.textureSlot >= ParticleMaterial::kNumEffectTextures)
				return Fail("노드 n" + std::to_string(id) + ": 텍스처 슬롯 " + std::to_string(n.textureSlot) + " 는 없음 (0 ~ 3)");

			InputInfo in[3];
			for (uint32_t k = 0; k < need; ++k)
			{
				const MaterialLink link = (k < n.inputs.size()) ? n.inputs[k] : MaterialLink();
				if (link.node < 0)
				{
					if (!DefaultInput(id, k, in[k]))
						return -1;
					continue;
				}

				const int reg = Compile(link.node);
				if (reg < 0)
					return -1;

				const uint32_t srcWidth = m_Width[link.node];
				uint32_t width = srcWidth;
				if (IsSingleComponent(link.swizzle))
					width = 1;
				else if (link.width != 0)
				{
					if (link.width > srcWidth || link.width > 4)
						return Fail(NodeText(id) + " 입력 " + PinName(k) + ": n" + std::to_string(link.node) + " 는 "
							+ WidthName(srcWidth) + " 인데 width " + std::to_string(link.width) + " 로 읽음");
					width = link.width;
				}
				in[k].reg = static_cast<uint32_t>(reg);
				in[k].swz = link.swizzle & 0xFFu;
				in[k].width = width;
			}

			const uint32_t outWidth = CheckTypes(id, in, need);
			if (outWidth == 0)
				return -1;

			for (uint32_t k = 0; k < need; ++k)
			{
				if (in[k].width != 1)
					continue;
				const uint32_t expanded = Broadcast(in[k].swz);
				if (expanded != in[k].swz && !in[k].autoFilled)
				{
					m_Notes.push_back(NodeText(id) + " 입력 " + PinName(k) + ": 숫자 하나(float)를 4 자리에 채움 (."
						+ std::string(4, "xyzw"[expanded & 3]) + ")");
					in[k].swz = expanded;
				}
			}

			MaterialInstr I = {};
			I.op = static_cast<uint32_t>(OpOf(n.type));
			I.swz = 0xE4E4E4u;
			I.srcA = in[0].reg; I.srcB = in[1].reg; I.srcC = in[2].reg;
			for (uint32_t k = 0; k < need; ++k)
			{
				const uint32_t shift = 8u * k;
				I.swz = (I.swz & ~(0xFFu << shift)) | (in[k].swz << shift);
			}

			const int dst = Emit(I, id, -1);
			if (dst < 0)
				return -1;
			m_RegOf[id] = dst;
			m_Width[id] = outWidth;
			return dst;
		}

		const MaterialGraph&        m_Graph;
		std::vector<int>            m_RegOf;
		std::vector<uint32_t>       m_Width;
		std::vector<MaterialInstr>  m_Instrs;
		std::vector<int>            m_InstrNode;
		std::vector<int>            m_InstrAuto;
		std::vector<float4>         m_AutoValues;
		int                         m_AutoQuadUV = -1;
		std::vector<std::string>    m_Notes;
		std::string                 m_Error;
	};
}

MaterialCompileResult CompileMaterialGraph(const MaterialGraph& graph)
{
	GraphCompiler compiler(graph);
	return compiler.Run();
}

namespace
{
	struct GraphWriter
	{
		MaterialGraph graph;

		int Add(MaterialNodeType type, std::vector<MaterialLink> inputs = {})
		{
			MaterialNode n;
			n.type = type;
			n.inputs = std::move(inputs);
			graph.nodes.push_back(n);
			return static_cast<int>(graph.nodes.size()) - 1;
		}
		int Constant(const float4& value)
		{
			const int id = Add(MaterialNodeType::Constant);
			graph.nodes[id].values[0] = value;
			return id;
		}
		int UVTransform(int uv, const float4& tilingOffset, const float4& scroll)
		{
			const int id = Add(MaterialNodeType::UVTransform, { L(uv) });
			graph.nodes[id].values[0] = tilingOffset;
			graph.nodes[id].values[1] = scroll;
			return id;
		}
		int Sample(int uv, uint32_t slot)
		{
			const int id = Add(MaterialNodeType::TextureSample, { L(uv) });
			graph.nodes[id].textureSlot = slot;
			return id;
		}
		MaterialGraph Finish(int output)
		{
			graph.output = output;
			return graph;
		}

		static MaterialLink L(int node, uint32_t swizzle = MaterialSwizzle::XYZW, uint32_t width = 0)
		{
			MaterialLink l;
			l.node = node;
			l.swizzle = swizzle;
			l.width = width;
			return l;
		}
	};
}

MaterialGraph BuildEffectGraph(ParticleMaterial::Effect effect)
{
	using namespace MaterialSwizzle;
	using NT = MaterialNodeType;
	auto D = [effect](uint32_t k) { return ParticleMaterial::DefaultParam(k, effect); };
	auto L = [](int node, uint32_t swz = XYZW, uint32_t width = 0) { return GraphWriter::L(node, swz, width); };

	GraphWriter g;
	switch (effect)
	{
	case ParticleMaterial::Effect::Pattern:
	case ParticleMaterial::Effect::Tint:
	{
		const int q    = g.Add(NT::QuadUV);
		const int uv   = g.UVTransform(q, D(0), D(1));
		const int tex  = g.Sample(uv, 0);
		const int base = g.Add(NT::BaseColor);
		const int mul  = g.Add(NT::Multiply, { L(base), L(tex, effect == ParticleMaterial::Effect::Pattern ? X : XYZW) });
		const int out  = g.Add(NT::Append, { L(mul), L(base, W) });
		return g.Finish(out);
	}
	case ParticleMaterial::Effect::Dissolve:
	{
		const int q    = g.Add(NT::QuadUV);
		const int uv   = g.UVTransform(q, D(0), D(1));
		const int mask = g.Sample(uv, 0);
		const int age  = g.Add(NT::ParticleParam);
		const int c2   = g.Constant(D(2));
		const int n4   = g.Add(NT::Add, { L(age, X), L(c2, X) });
		const int n5   = g.Add(NT::Smoothstep, { L(age, X), L(n4, X), L(mask, X) });
		const int base = g.Add(NT::BaseColor);
		const int n6   = g.Add(NT::Multiply, { L(base), L(n5, X) });
		const int n7   = g.Add(NT::Add, { L(n4, X), L(c2, Y) });
		const int s    = g.Add(NT::Smoothstep, { L(n4, X), L(n7, X), L(mask, X) });
		const int n8   = g.Add(NT::OneMinus, { L(s) });
		const int cov  = g.Add(NT::Coverage);
		const int t0   = g.Add(NT::Multiply, { L(n8, X), L(n5, X) });
		const int t1   = g.Add(NT::Multiply, { L(t0, X), L(cov, X) });
		const int c3   = g.Constant(D(3));
		const int edge = g.Add(NT::Multiply, { L(c3), L(t1, X) });
		const int n9   = g.Add(NT::Add, { L(n6), L(edge) });
		const int out  = g.Add(NT::Append, { L(n9), L(n6, W) });
		return g.Finish(out);
	}
	case ParticleMaterial::Effect::Distortion:
	{
		const int q    = g.Add(NT::QuadUV);
		const int uv   = g.UVTransform(q, D(0), D(1));
		const int n2   = g.Sample(uv, 0);
		const int sn   = g.Add(NT::UnpackSigned, { L(n2) });
		const int c2   = g.Constant(D(2));
		const int off  = g.Add(NT::Multiply, { L(sn), L(c2) });
		const int base = g.Add(NT::BaseSample, { L(off, XYZW, 2) });
		const int n3   = g.Add(NT::Add, { L(q), L(off, XYZW, 2) });
		const int n4   = g.Sample(n3, 1);
		const int mul  = g.Add(NT::Multiply, { L(base), L(n4, X) });
		const int out  = g.Add(NT::Append, { L(mul), L(base, W) });
		return g.Finish(out);
	}
	case ParticleMaterial::Effect::Glow:
	{
		const int q    = g.Add(NT::QuadUV);
		const int uv0  = g.UVTransform(q, D(0), D(1));
		const int n2   = g.Sample(uv0, 0);
		const int uv1  = g.UVTransform(q, D(2), D(3));
		const int m2   = g.Sample(uv1, 1);
		const int n3   = g.Add(NT::Multiply, { L(n2, X), L(m2, X) });
		const int cov  = g.Add(NT::Coverage);
		const int t0   = g.Add(NT::Multiply, { L(n3, X), L(cov, X) });
		const int c4   = g.Constant(D(4));
		const int glow = g.Add(NT::Multiply, { L(c4), L(t0, X) });
		const int base = g.Add(NT::BaseColor);
		const int n4   = g.Add(NT::Add, { L(base), L(glow) });
		const int out  = g.Add(NT::Append, { L(n4), L(base, W) });
		return g.Finish(out);
	}
	default:
		return g.Finish(-1);
	}
}

namespace
{
	std::string LinkText(const MaterialLink& l)
	{
		if (l.node < 0)
			return "(empty)";
		std::string s = "n" + std::to_string(l.node);
		switch (l.swizzle)
		{
		case MaterialSwizzle::XYZW: break;
		case MaterialSwizzle::X:    s += ".x"; break;
		case MaterialSwizzle::Y:    s += ".y"; break;
		case MaterialSwizzle::Z:    s += ".z"; break;
		case MaterialSwizzle::W:    s += ".w"; break;
		default:
		{
			const char kComp[4] = { 'x', 'y', 'z', 'w' };
			s += '.';
			for (uint32_t c = 0; c < 4; ++c)
				s += kComp[(l.swizzle >> (2 * c)) & 3];
			break;
		}
		}
		if (l.width != 0)
			s += "[w" + std::to_string(l.width) + "]";
		return s;
	}

	std::string F4(const float4& v)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "(%g, %g, %g, %g)", v.x, v.y, v.z, v.w);
		return buf;
	}
}

std::string DumpMaterialGraph(const MaterialGraph& graph, const MaterialCompileResult* compiled)
{
	if (graph.output < 0)
		return "(no graph = base color)\n";

	std::string out;
	char line[96];
	for (size_t id = 0; id < graph.nodes.size(); ++id)
	{
		const MaterialNode& n = graph.nodes[id];
		snprintf(line, sizeof(line), "n%-2u %-14s", uint32_t(id), MaterialNodeTypeName(n.type));
		out += line;

		if (compiled && compiled->ok && id < compiled->nodeWidth.size())
		{
			const uint32_t w = compiled->nodeWidth[id];
			out += std::string(" : ") + (w == 0 ? "-     " : w == 1 ? "float " : w == 2 ? "float2" : w == 3 ? "float3" : "float4");
		}

		std::string args;
		for (const MaterialLink& l : n.inputs)
			args += (args.empty() ? "" : ", ") + LinkText(l);
		if (!args.empty())
			out += " <- " + args;

		if (n.type == MaterialNodeType::Constant)
			out += "  value " + F4(n.values[0]);
		else if (n.type == MaterialNodeType::UVTransform)
			out += "  tiling/offset " + F4(n.values[0]) + " scroll " + F4(n.values[1]);
		else if (n.type == MaterialNodeType::TextureSample)
			out += "  Tex" + std::to_string(n.textureSlot);
		out += "\n";
	}
	out += "output = n" + std::to_string(graph.output) + "\n";

	if (compiled)
	{
		if (!compiled->ok)
			out += "compile error: " + compiled->error + "\n";
		for (const std::string& note : compiled->notes)
			out += "note: " + note + "\n";
	}
	return out;
}
