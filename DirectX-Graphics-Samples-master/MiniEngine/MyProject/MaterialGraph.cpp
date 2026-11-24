#include "pch.h"
#include "MaterialGraph.h"
#include "Json.h"

#include <algorithm>
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
	case MaterialNodeType::UVTransform: 
	case MaterialNodeType::TextureSample: 
	case MaterialNodeType::Saturate:
	case MaterialNodeType::OneMinus: 
	case MaterialNodeType::UnpackSigned:
	case MaterialNodeType::BaseSample:
		return 1;
	case MaterialNodeType::Add: 
	case MaterialNodeType::Subtract:
	case MaterialNodeType::Multiply: 
	case MaterialNodeType::Append:
		return 2;
	case MaterialNodeType::Lerp: 
	case MaterialNodeType::Smoothstep:
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

			if (m_Width[m_Graph.output] != 4) // return은 무조건 Color
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

			uint32_t regCount = 0;
			const int outputPhys = AllocateRegisters(static_cast<uint32_t>(outputReg), regCount);
			if (outputPhys < 0)
				return Failed(result);
			result.regCount = regCount;

			result.ok = true;
			result.program.instrs = m_Instrs;
			result.program.outputReg = static_cast<uint32_t>(outputPhys);
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
			result.regCount = 0;
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

		// Emit시에 
		int Emit(MaterialInstr I, int node, int autoIndex, uint32_t inputCount)
		{
			if (m_Instrs.size() >= MaterialProgram::kMaxInstrs)
				return Fail("명령어가 " + std::to_string(MaterialProgram::kMaxInstrs) + " 개를 넘음");
			I.dst = static_cast<uint32_t>(m_Instrs.size());
			m_Instrs.push_back(I);
			m_InstrNode.push_back(node);
			m_InstrAuto.push_back(autoIndex);
			m_InstrInputs.push_back(inputCount);
			return static_cast<int>(I.dst);

		}

		// outputVirtual = 출력 노드의 임시 번호 / 반환 = 출력의 실제 칸, 실패하면 -1
		int AllocateRegisters(uint32_t outputVirtual, uint32_t& regCount)
		{
			const size_t count = m_Instrs.size();
			const size_t kNever = static_cast<size_t>(-1);

			// 1) 값마다 마지막으로 읽는 명령어
			std::vector<size_t> lastUse(count, kNever);
			for (size_t i = 0; i < count; ++i)
			{
				const uint32_t src[3] = { m_Instrs[i].srcA, m_Instrs[i].srcB, m_Instrs[i].srcC };
				for (uint32_t j = 0; j < m_InstrInputs[i]; ++j)
					lastUse[src[j]] = i;
			}

			// 2) 앞에서부터 칸 배정
			std::vector<uint32_t> phys(count, 0);
			bool busy[MaterialProgram::kMaxRegs] = {};
			regCount = 0;

			for (size_t i = 0; i < count; ++i)
			{
				MaterialInstr& In = m_Instrs[i];
				uint32_t* src[3] = { &In.srcA, &In.srcB, &In.srcC };
				uint32_t original[3] = { In.srcA, In.srcB, In.srcC };

				for (uint32_t j = 0; j < 3; ++j)
					*src[j] = (j < m_InstrInputs[i]) ? phys[original[j]] : 0u;

				for (uint32_t j = 0; j < m_InstrInputs[i]; ++j)
				{
					if (lastUse[original[j]] == i && original[j] != outputVirtual)
						busy[phys[original[j]]] = false;
				}

				// 결과: 비어 있는 가장 앞 칸
				uint32_t r = 0;
				while (r < MaterialProgram::kMaxRegs && busy[r])
					++r;
				if (r == MaterialProgram::kMaxRegs)
				{
					const std::string who = (m_InstrNode[i] >= 0) ? NodeText(m_InstrNode[i]) : std::string("빈 핀 기본값");
					Fail("동시에 살아 있는 값이 " + std::to_string(MaterialProgram::kMaxRegs) + " 개를 넘음 (" + who
						+ "). 앞에서 만든 값을 한참 뒤에서 쓰는 연결이 많으면 이렇게 됩니다");
					return -1;
				}
				phys[i] = r;
				In.dst = r;
				regCount = (std::max)(regCount, r + 1);

				// 아무도 안 읽는 값(출력 제외)은 쓰자마자 비운다
				busy[r] = (lastUse[i] != kNever) || (i == outputVirtual);
			}
			return static_cast<int>(phys[outputVirtual]);
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
					m_AutoQuadUV = Emit(I, -1, -1, 0);
				}
				reg = m_AutoQuadUV;
			}
			else
			{
				MaterialInstr I = {};
				I.op = static_cast<uint32_t>(MaterialOp::Const);
				I.swz = 0xE4E4E4u;
				m_AutoValues.push_back(d.value);
				reg = Emit(I, -1, static_cast<int>(m_AutoValues.size()) - 1, 0);
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
				return Fail(NodeText(id) + ": 입력은 " + std::to_string(need) + " 개 까지인데 " + std::to_string(n.inputs.size()) + " 개");
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

			const int dst = Emit(I, id, -1, need);

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
		std::vector<uint32_t>       m_InstrInputs;
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

//
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

		return g.Finish(base);
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


MaterialGraph BuildLayeredEffectGraph(const std::vector<ParticleMaterial::Effect>& layers)
{
	MaterialGraph graph;
	int previousOutput = -1;
	int previousUV = -1;
	uint32_t textureBase = 0;

	for (const ParticleMaterial::Effect effect : layers)
	{
		const MaterialGraph layer = BuildEffectGraph(effect);
		if (layer.output < 0)
			continue;

		const int offset = static_cast<int>(graph.nodes.size());
		for (MaterialNode node : layer.nodes)
		{
			if (node.type == MaterialNodeType::TextureSample)
				node.textureSlot += textureBase;				// 텍스처 슬롯

			for (MaterialLink& link : node.inputs)
			{
				if (link.node < 0)
					continue;

				const bool readsBase = (layer.nodes[link.node].type == MaterialNodeType::BaseColor);
				link.node = (readsBase && previousOutput >= 0) ? previousOutput : link.node + offset; // 각 노드가 읽는 값이 offset만큼 밀려남
			}
			graph.nodes.push_back(node);
		}

		previousOutput = layer.output + offset;
		textureBase += ParticleMaterial::NumEffectTextures(effect);
	}
	graph.output = previousOutput;

	return graph;
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
		if (compiled->ok)
			out += "instrs " + std::to_string(compiled->program.instrs.size()) + " / " + std::to_string(MaterialProgram::kMaxInstrs)
			+ ", regs " + std::to_string(compiled->regCount) + " / " + std::to_string(MaterialProgram::kMaxRegs) + "\n";

	}
	return out;
}

const char* MaterialNodePinName(MaterialNodeType type, uint32_t pin)
{
	switch (type)
	{
	case MaterialNodeType::UVTransform:
	case MaterialNodeType::TextureSample: return "uv";
	case MaterialNodeType::BaseSample:    return "offset";
	case MaterialNodeType::Saturate:
	case MaterialNodeType::OneMinus:
	case MaterialNodeType::UnpackSigned:  return "in";
	case MaterialNodeType::Append:        return pin == 0 ? "rgb" : "a";
	case MaterialNodeType::Lerp:          return pin == 0 ? "a" : (pin == 1 ? "b" : "t");
	case MaterialNodeType::Smoothstep:    return pin == 0 ? "min" : (pin == 1 ? "max" : "x");
	default:                              return pin == 0 ? "a" : (pin == 1 ? "b" : "c");
	}
}

bool MaterialNodeTypeFromName(const std::string& name, MaterialNodeType& out)
{
	for (uint32_t t = 0; t < uint32_t(MaterialNodeType::Count); ++t)
	{
		if (name == MaterialNodeTypeName(MaterialNodeType(t)))
		{
			out = MaterialNodeType(t);
			return true;
		}
	}
	return false;
}

bool HasMaterialGraphLayout(const MaterialGraph& graph)
{
	for (const MaterialNode& n : graph.nodes)
	{
		if (n.pos.x != 0.f || n.pos.y != 0.f)
			return true;
	}
	return false;
}

void LayoutMaterialGraph(MaterialGraph& graph)
{
	const int count = static_cast<int>(graph.nodes.size());
	std::vector<int> depth(count, -1);
	if (graph.output >= 0 && graph.output < count)
		depth[graph.output] = 0;
	for (int pass = 0; pass < count; ++pass)
	{
		bool changed = false;
		for (int id = 0; id < count; ++id)
		{
			if (depth[id] < 0)
				continue;
			for (const MaterialLink& l : graph.nodes[id].inputs)
			{
				if (l.node < 0 || l.node >= count || depth[l.node] >= depth[id] + 1)
					continue;
				depth[l.node] = depth[id] + 1;
				changed = true;
			}
		}
		if (!changed)
			break;
	}

	const float kColumn = 230.f, kRow = 120.f;
	int maxDepth = 0;
	for (int d : depth)
		maxDepth = (std::max)(maxDepth, d);
	std::vector<int> rows(maxDepth + 2, 0);
	for (int id = 0; id < count; ++id)
	{
		const int column = (depth[id] < 0) ? 0 : (maxDepth - depth[id] + 1);
		const int row = rows[column]++;
		graph.nodes[id].pos = { 40.f + column * kColumn, 40.f + row * kRow };
	}
}

MaterialGraph KeepMaterialGraphNodes(const MaterialGraph& graph, const std::vector<bool>& keep)
{
	std::vector<int> remap(graph.nodes.size(), -1);
	MaterialGraph out;
	for (size_t id = 0; id < graph.nodes.size(); ++id)
	{
		if (id < keep.size() && !keep[id])
			continue;
		remap[id] = static_cast<int>(out.nodes.size());
		out.nodes.push_back(graph.nodes[id]);
	}
	for (MaterialNode& n : out.nodes)
	{
		for (MaterialLink& l : n.inputs)
		{
			if (l.node >= 0)
				l.node = (l.node < static_cast<int>(remap.size())) ? remap[l.node] : -1;
		}
	}
	out.output = (graph.output >= 0 && graph.output < static_cast<int>(remap.size())) ? remap[graph.output] : -1;
	return out;
}

MaterialGraph PruneMaterialGraph(const MaterialGraph& graph)
{
	const int count = static_cast<int>(graph.nodes.size());
	std::vector<bool> keep(count, false);
	std::vector<int> stack;
	if (graph.output >= 0 && graph.output < count)
		stack.push_back(graph.output);
	while (!stack.empty())
	{
		const int id = stack.back();
		stack.pop_back();
		if (keep[id])
			continue;
		keep[id] = true;
		for (const MaterialLink& l : graph.nodes[id].inputs)
		{
			if (l.node >= 0 && l.node < count && !keep[l.node])
				stack.push_back(l.node);
		}
	}
	return KeepMaterialGraphNodes(graph, keep);
}

bool FillMaterialGraphValues(MaterialGraph& graph, const float4* params, uint32_t count)
{
	const MaterialCompileResult compiled = CompileMaterialGraph(graph);
	if (!compiled.ok)
		return false;
	uint32_t slot = 0;
	for (size_t id = 0; id < graph.nodes.size(); ++id)
	{
		if (id >= compiled.nodeWidth.size() || compiled.nodeWidth[id] == 0)
			continue;
		MaterialNode& n = graph.nodes[id];
		for (uint32_t k = 0; k < ParamSlotsOf(n.type); ++k, ++slot)
		{
			if (slot < count)
				n.values[k] = params[slot];
		}
	}
	return true;
}

std::string MaterialSwizzleText(uint32_t swizzle)
{
	const char kComp[4] = { 'x', 'y', 'z', 'w' };
	swizzle &= 0xFFu;
	if (swizzle == (swizzle & 3u) * 0x55u)
		return std::string(1, kComp[swizzle & 3u]);
	std::string s;
	for (uint32_t c = 0; c < 4; ++c)
		s += kComp[(swizzle >> (2 * c)) & 3u];
	return s;
}

bool MaterialSwizzleFromText(const std::string& text, uint32_t& out)
{
	auto Comp = [](char c) -> int { return c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : c == 'w' ? 3 : -1; };
	if (text.size() == 1 && Comp(text[0]) >= 0)
	{
		out = uint32_t(Comp(text[0])) * 0x55u;
		return true;
	}
	if (text.size() != 4)
		return false;
	uint32_t s = 0;
	for (uint32_t c = 0; c < 4; ++c)
	{
		const int v = Comp(text[c]);
		if (v < 0)
			return false;
		s |= uint32_t(v) << (2 * c);
	}
	out = s;
	return true;
}

namespace
{
	JsonValue Float4Json(const float4& v)
	{
		JsonValue a = JsonValue::MakeArray();
		a.Push(JsonValue(v.x));
		a.Push(JsonValue(v.y));
		a.Push(JsonValue(v.z));
		a.Push(JsonValue(v.w));
		return a;
	}

	bool JsonFloats(const JsonValue* v, float* out, uint32_t count)
	{
		if (v == nullptr || !v->IsArray() || v->Size() != count)
			return false;
		for (uint32_t i = 0; i < count; ++i)
		{
			if (!v->At(i).IsNumber())
				return false;
			out[i] = static_cast<float>(v->At(i).AsNumber());
		}
		return true;
	}
}

JsonValue MaterialGraphToJson(const MaterialGraph& graph)
{
	JsonValue root = JsonValue::MakeObject();
	root.Set("output", JsonValue(graph.output));
	JsonValue nodes = JsonValue::MakeArray();
	for (const MaterialNode& n : graph.nodes)
	{
		JsonValue jn = JsonValue::MakeObject();
		jn.Set("type", JsonValue(MaterialNodeTypeName(n.type)));
		JsonValue pos = JsonValue::MakeArray();
		pos.Push(JsonValue(n.pos.x));
		pos.Push(JsonValue(n.pos.y));
		jn.Set("pos", std::move(pos));

		JsonValue inputs = JsonValue::MakeArray();
		for (const MaterialLink& l : n.inputs)
		{
			JsonValue jl = JsonValue::MakeObject();
			jl.Set("node", JsonValue(l.node));
			if ((l.swizzle & 0xFFu) != MaterialSwizzle::XYZW)
				jl.Set("swizzle", JsonValue(MaterialSwizzleText(l.swizzle)));
			if (l.width != 0)
				jl.Set("width", JsonValue(l.width));
			inputs.Push(std::move(jl));
		}
		jn.Set("inputs", std::move(inputs));

		if (n.type == MaterialNodeType::Constant)
			jn.Set("value", Float4Json(n.values[0]));
		else if (n.type == MaterialNodeType::UVTransform)
		{
			jn.Set("tilingOffset", Float4Json(n.values[0]));
			jn.Set("scroll", Float4Json(n.values[1]));
		}
		else if (n.type == MaterialNodeType::TextureSample)
			jn.Set("slot", JsonValue(n.textureSlot));
		nodes.Push(std::move(jn));
	}
	root.Set("nodes", std::move(nodes));
	return root;
}

bool MaterialGraphFromJson(const JsonValue& json, MaterialGraph& out, std::vector<std::string>& warnings)
{
	const JsonValue* nodes = json.Find("nodes");
	if (!json.IsObject() || nodes == nullptr || !nodes->IsArray())
	{
		warnings.push_back("materialGraph: expected { \"output\": n, \"nodes\": [...] } -> no graph");
		return false;
	}

	MaterialGraph g;
	const int count = static_cast<int>(nodes->Size());
	for (int id = 0; id < count; ++id)
	{
		const JsonValue& jn = nodes->At(id);
		const std::string where = "materialGraph.nodes[" + std::to_string(id) + "]";
		MaterialNode n;
		const JsonValue* type = jn.Find("type");
		if (type == nullptr || !MaterialNodeTypeFromName(type->AsString(), n.type))
		{
			warnings.push_back(where + ": unknown type '" + (type ? type->AsString() : std::string()) + "' -> Constant 0");
			n.type = MaterialNodeType::Constant;
		}
		float pos[2] = {};
		if (JsonFloats(jn.Find("pos"), pos, 2))
			n.pos = { pos[0], pos[1] };

		if (const JsonValue* inputs = jn.Find("inputs"))
		{
			const uint32_t need = MaterialNodeInputCount(n.type);
			if (inputs->Size() > need)
				warnings.push_back(where + ": more inputs than " + std::to_string(need) + " -> extra ignored");
			for (uint32_t k = 0; k < (std::min)(uint32_t(inputs->Size()), need); ++k)
			{
				const JsonValue& jl = inputs->At(k);
				MaterialLink l;
				if (const JsonValue* v = jl.Find("node"))
					l.node = static_cast<int>(v->AsNumber(-1.0));
				if (l.node >= count || l.node < -1)
				{
					warnings.push_back(where + ".inputs[" + std::to_string(k) + "]: node " + std::to_string(l.node) + " does not exist -> empty");
					l.node = -1;
				}
				if (const JsonValue* v = jl.Find("swizzle"))
				{
					if (!MaterialSwizzleFromText(v->AsString(), l.swizzle))
					{
						warnings.push_back(where + ".inputs[" + std::to_string(k) + "]: bad swizzle '" + v->AsString() + "' -> xyzw");
						l.swizzle = MaterialSwizzle::XYZW;
					}
				}
				if (const JsonValue* v = jl.Find("width"))
					l.width = (std::min)(static_cast<uint32_t>(v->AsNumber(0.0)), 4u);
				n.inputs.push_back(l);
			}
		}

		if (n.type == MaterialNodeType::Constant)
			JsonFloats(jn.Find("value"), &n.values[0].x, 4);
		else if (n.type == MaterialNodeType::UVTransform)
		{
			JsonFloats(jn.Find("tilingOffset"), &n.values[0].x, 4);
			JsonFloats(jn.Find("scroll"), &n.values[1].x, 4);
		}
		else if (n.type == MaterialNodeType::TextureSample)
		{
			if (const JsonValue* v = jn.Find("slot"))
				n.textureSlot = (std::min)(static_cast<uint32_t>(v->AsNumber(0.0)), ParticleMaterial::kNumEffectTextures - 1);
		}
		g.nodes.push_back(n);
	}

	g.output = -1;
	if (const JsonValue* v = json.Find("output"))
		g.output = static_cast<int>(v->AsNumber(-1.0));
	if (g.output >= count || g.output < -1)
	{
		warnings.push_back("materialGraph.output: node " + std::to_string(g.output) + " does not exist -> no output");
		g.output = -1;
	}
	out = std::move(g);
	return true;
}
