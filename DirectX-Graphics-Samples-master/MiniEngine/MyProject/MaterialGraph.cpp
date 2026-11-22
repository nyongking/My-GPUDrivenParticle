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

	class GraphCompiler
	{
	public:
		explicit GraphCompiler(const MaterialGraph& graph)
			: m_Graph(graph), m_RegOf(graph.nodes.size(), kNotYet)
		{
		}

		MaterialCompileResult Run()
		{
			MaterialCompileResult result;

			if (m_Graph.output < 0)
			{
				result.ok = true;
				return result;
			}

			const int outputReg = Compile(m_Graph.output);
			if (outputReg < 0)
				return Failed(result);

			std::vector<int> slotOf(m_Graph.nodes.size(), -1);
			for (size_t id = 0; id < m_Graph.nodes.size(); ++id)
			{
				if (m_RegOf[id] < 0)
					continue;
				const MaterialNode& n = m_Graph.nodes[id];
				const uint32_t count = ParamSlotsOf(n.type);
				if (count == 0)
					continue;
				if (result.params.size() + count > ParticleMaterial::kMaxParams)
				{
					m_Error = "값 칸이 " + std::to_string(ParticleMaterial::kMaxParams) + " 개를 넘음 (노드 n" + std::to_string(id) + ")";
					return Failed(result);
				}
				slotOf[id] = static_cast<int>(result.params.size());
				for (uint32_t k = 0; k < count; ++k)
					result.params.push_back(n.values[k]);
			}

			for (size_t k = 0; k < m_Instrs.size(); ++k)
			{
				const MaterialNode& n = m_Graph.nodes[m_InstrNode[k]];
				if (ParamSlotsOf(n.type) > 0)
					m_Instrs[k].param = static_cast<uint32_t>(slotOf[m_InstrNode[k]]);
				else if (n.type == MaterialNodeType::TextureSample)
					m_Instrs[k].param = n.textureSlot;
			}

			result.ok = true;
			result.program.instrs = m_Instrs;
			result.program.outputReg = static_cast<uint32_t>(outputReg);
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
			return result;
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
			if (n.inputs.size() != need)
				return Fail("노드 n" + std::to_string(id) + " (" + MaterialNodeTypeName(n.type) + "): 입력 " + std::to_string(need)
					+ " 개가 필요한데 " + std::to_string(n.inputs.size()) + " 개");
			if (n.type == MaterialNodeType::TextureSample && n.textureSlot >= ParticleMaterial::kNumEffectTextures)
				return Fail("노드 n" + std::to_string(id) + ": 텍스처 슬롯 " + std::to_string(n.textureSlot) + " 는 없음 (0 ~ 3)");

			MaterialInstr I = {};
			I.op = static_cast<uint32_t>(OpOf(n.type));
			I.swz = MaterialSwizzle::XYZW | (MaterialSwizzle::XYZW << 8) | (MaterialSwizzle::XYZW << 16);

			uint32_t src[3] = {};
			for (size_t k = 0; k < n.inputs.size(); ++k)
			{
				const int reg = Compile(n.inputs[k].node);
				if (reg < 0)
					return -1;
				src[k] = static_cast<uint32_t>(reg);
				const uint32_t shift = static_cast<uint32_t>(8 * k);
				I.swz = (I.swz & ~(0xFFu << shift)) | ((n.inputs[k].swizzle & 0xFFu) << shift);
			}
			I.srcA = src[0]; I.srcB = src[1]; I.srcC = src[2];

			if (m_Instrs.size() >= MaterialProgram::kMaxInstrs)
				return Fail("명령어가 " + std::to_string(MaterialProgram::kMaxInstrs) + " 개를 넘음");
			if (m_Instrs.size() >= MaterialProgram::kMaxRegs)
				return Fail("칸이 " + std::to_string(MaterialProgram::kMaxRegs) + " 개를 넘음 (칸 재사용은 아직 없음)");

			I.dst = static_cast<uint32_t>(m_Instrs.size());
			m_Instrs.push_back(I);
			m_InstrNode.push_back(id);

			m_RegOf[id] = static_cast<int>(I.dst);
			return m_RegOf[id];
		}

		const MaterialGraph&        m_Graph;
		std::vector<int>            m_RegOf;
		std::vector<MaterialInstr>  m_Instrs;
		std::vector<int>            m_InstrNode;
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

		static MaterialLink L(int node, uint32_t swizzle = MaterialSwizzle::XYZW)
		{
			MaterialLink l;
			l.node = node;
			l.swizzle = swizzle;
			return l;
		}
	};
}

MaterialGraph BuildEffectGraph(ParticleMaterial::Effect effect)
{
	using namespace MaterialSwizzle;
	using NT = MaterialNodeType;
	auto D = [effect](uint32_t k) { return ParticleMaterial::DefaultParam(k, effect); };
	auto L = [](int node, uint32_t swz = XYZW) { return GraphWriter::L(node, swz); };

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
		const int base = g.Add(NT::BaseSample, { L(off) });
		const int n3   = g.Add(NT::Add, { L(q), L(off) });
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
		return s;
	}

	std::string F4(const float4& v)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "(%g, %g, %g, %g)", v.x, v.y, v.z, v.w);
		return buf;
	}
}

std::string DumpMaterialGraph(const MaterialGraph& graph)
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
	return out;
}
