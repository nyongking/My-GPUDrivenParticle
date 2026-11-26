#include "pch.h"
#include "MaterialGraphEditor.h"

#include "imgui.h"
#include "imnodes.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace
{
	const int kPinStride = 8;
	const int kOutPin = 7;

	int InPin(size_t node, uint32_t pin) { return static_cast<int>(node) * kPinStride + static_cast<int>(pin); }
	int OutPin(size_t node) { return static_cast<int>(node) * kPinStride + kOutPin; }
	bool IsOutPin(int pin) { return pin % kPinStride == kOutPin; }

	bool s_ContextReady = false;
	std::map<std::string, ImNodesEditorContext*> s_Editors;

	struct UiState
	{
		ImVec2 addPos = ImVec2(0.f, 0.f);
		int    menuNode = -1;
	};
	std::map<std::string, UiState> s_Ui;

	struct SwizzleOption
	{
		const char* name;
		uint32_t    swizzle;
		uint32_t    width;
	};
	const SwizzleOption kSwizzles[] = {
		{ "all", MaterialSwizzle::XYZW, 0 },
		{ ".xy", MaterialSwizzle::XYZW, 2 },
		{ ".x",  MaterialSwizzle::X,    0 },
		{ ".y",  MaterialSwizzle::Y,    0 },
		{ ".z",  MaterialSwizzle::Z,    0 },
		{ ".w",  MaterialSwizzle::W,    0 },
	};

	int SwizzleIndex(const MaterialLink& l)
	{
		for (int i = 0; i < IM_ARRAYSIZE(kSwizzles); ++i)
		{
			if ((l.swizzle & 0xFFu) == kSwizzles[i].swizzle && l.width == kSwizzles[i].width)
				return i;
		}
		return -1;
	}

	const char* WidthText(uint32_t width)
	{
		switch (width)
		{
		case 1:  return "float";
		case 2:  return "float2";
		case 3:  return "float3";
		case 4:  return "float4";
		default: return "";
		}
	}

	int ErrorNode(const std::string& error)
	{
		for (size_t i = 0; i + 1 < error.size(); ++i)
		{
			if (error[i] != 'n' || !std::isdigit(static_cast<unsigned char>(error[i + 1])))
				continue;
			if (i > 0 && std::isalnum(static_cast<unsigned char>(error[i - 1])))
				continue;
			return std::atoi(error.c_str() + i + 1);
		}
		return -1;
	}

	void TrimInputs(MaterialNode& n)
	{
		while (!n.inputs.empty() && n.inputs.back().node < 0)
			n.inputs.pop_back();
	}

	// 숫자 입력 (성분 comps 개). 드래그하는 동안 kValue, 값을 놓으면 kGraph
	uint32_t DragValue(const char* id, float* v, uint32_t comps, bool editable)
	{
		comps = (std::max)(1u, (std::min)(comps, 4u));
		if (!editable)
		{
			std::string text;
			char buf[32];
			for (uint32_t c = 0; c < comps; ++c)
			{
				snprintf(buf, sizeof(buf), "%s%.2f", c ? ", " : "", v[c]);
				text += buf;
			}
			if (comps > 1)
				text = "(" + text + ")";
			ImGui::TextUnformatted(text.c_str());
			return MaterialGraphEditor::kNoChange;
		}

		uint32_t change = MaterialGraphEditor::kNoChange;
		ImGui::SetNextItemWidth(20.f + 38.f * static_cast<float>(comps));
		if (ImGui::DragScalarN(id, ImGuiDataType_Float, v, static_cast<int>(comps), 0.01f, nullptr, nullptr, "%.2f"))
			change |= MaterialGraphEditor::kValue;
		if (ImGui::IsItemDeactivatedAfterEdit())
			change |= MaterialGraphEditor::kGraph;
		return change;
	}

	uint32_t DrawNodeValues(MaterialNode& n, bool editable)
	{
		uint32_t changed = MaterialGraphEditor::kNoChange;
		switch (n.type)
		{
		case MaterialNodeType::Constant:
		{
			// 값의 타입 (float ~ float4): 셰이더에 그 타입의 숫자로 들어간다
			static const char* const kWidths[] = { "float", "float2", "float3", "float4" };
			int width = static_cast<int>((std::max)(1u, (std::min)(n.width, 4u))) - 1;
			if (editable)
			{
				ImGui::SetNextItemWidth(80.f);
				if (ImGui::Combo("##width", &width, kWidths, IM_ARRAYSIZE(kWidths)))
				{
					n.width = static_cast<uint32_t>(width) + 1;
					changed |= MaterialGraphEditor::kGraph;
				}
			}
			changed |= DragValue("##value", &n.values[0].x, n.width, editable);
			break;
		}
		case MaterialNodeType::UVTransform:
			ImGui::TextDisabled("tiling.xy offset.zw");
			changed |= DragValue("##tile", &n.values[0].x, 4, editable);
			ImGui::TextDisabled("scroll.xy (/s)");
			changed |= DragValue("##scroll", &n.values[1].x, 2, editable);
			break;
		case MaterialNodeType::TextureSample:
		{
			static const char* const kSlots[] = { "Tex0", "Tex1", "Tex2", "Tex3" };
			int slot = static_cast<int>((std::min)(n.textureSlot, ParticleMaterial::kNumEffectTextures - 1));
			ImGui::SetNextItemWidth(80.f);
			if (editable)
			{
				if (ImGui::Combo("##slot", &slot, kSlots, IM_ARRAYSIZE(kSlots)))
				{
					n.textureSlot = static_cast<uint32_t>(slot);
					changed |= MaterialGraphEditor::kGraph;
				}
			}
			else
				ImGui::Text("%s", kSlots[slot]);
			break;
		}
		default:
			break;
		}
		return changed;
	}
}

namespace MaterialGraphEditor
{
	uint32_t Draw(const char* editorId, MaterialGraph& graph, const MaterialCompileResult& compiled, bool editable)
	{
		if (!s_ContextReady)
		{
			ImNodes::CreateContext();
			ImNodes::StyleColorsDark();
			s_ContextReady = true;
		}
		ImNodesEditorContext*& context = s_Editors[editorId];
		if (context == nullptr)
			context = ImNodes::EditorContextCreate();
		ImNodes::EditorContextSet(context);
		UiState& ui = s_Ui[editorId];

		uint32_t change = kNoChange;
		const int count = static_cast<int>(graph.nodes.size());
		if (!HasMaterialGraphLayout(graph))
		{
			LayoutMaterialGraph(graph);
			change |= kLayout;
		}

		if (!compiled.ok)
			ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "%s", compiled.error.c_str());
		else
			ImGui::TextDisabled("instrs %u / %u, regs %u / %u", uint32_t(compiled.program.instrs.size()), MaterialProgram::kMaxInstrs,
				compiled.regCount, MaterialProgram::kMaxRegs);
		ImGui::SameLine();
		ImGui::TextDisabled(editable ? "|  right click: add node / node menu,  Delete: remove selected,  drag pins to link"
			: "|  view only (built from layers)");
		const int errorNode = compiled.ok ? -1 : ErrorNode(compiled.error);

		ImGui::PushID(editorId);
		const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
		ImNodes::BeginNodeEditor();
		if (editable)
			ImNodes::PushAttributeFlag(ImNodesAttributeFlags_EnableLinkDetachWithDragClick);

		for (int id = 0; id < count; ++id)
		{
			MaterialNode& n = graph.nodes[id];
			const bool isOutput = (id == graph.output);
			const bool used = id < static_cast<int>(compiled.nodeWidth.size()) && compiled.nodeWidth[id] != 0;

			int colors = 0;
			if (id == errorNode)
			{
				ImNodes::PushColorStyle(ImNodesCol_TitleBar, IM_COL32(170, 45, 40, 255));
				ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, IM_COL32(200, 60, 50, 255));
				ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, IM_COL32(220, 70, 60, 255));
				colors = 3;
			}
			else if (isOutput)
			{
				ImNodes::PushColorStyle(ImNodesCol_TitleBar, IM_COL32(47, 91, 211, 255));
				ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, IM_COL32(70, 115, 230, 255));
				ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, IM_COL32(90, 135, 240, 255));
				colors = 3;
			}

			ImNodes::SetNodeGridSpacePos(id, ImVec2(n.pos.x, n.pos.y));
			ImNodes::SetNodeDraggable(id, editable);
			ImNodes::BeginNode(id);
			ImGui::PushID(id);

			ImNodes::BeginNodeTitleBar();
			ImGui::Text("%s%s", isOutput ? "OUT  " : "", MaterialNodeTypeName(n.type));
			if (used)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("%s", WidthText(compiled.nodeWidth[id]));
			}
			else if (compiled.ok)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("(unused)");
			}
			ImNodes::EndNodeTitleBar();

			const uint32_t inputCount = MaterialNodeInputCount(n.type);
			for (uint32_t k = 0; k < inputCount; ++k)
			{
				ImNodes::BeginInputAttribute(InPin(id, k));
				ImGui::TextUnformatted(MaterialNodePinName(n.type, k));
				const bool linked = k < n.inputs.size() && n.inputs[k].node >= 0;
				if (linked)
				{
					MaterialLink& l = n.inputs[k];
					const int current = SwizzleIndex(l);
					ImGui::SameLine();
					if (editable && current >= 0)
					{
						ImGui::PushID(static_cast<int>(k));
						ImGui::SetNextItemWidth(56.f);
						if (ImGui::BeginCombo("##swz", kSwizzles[current].name, ImGuiComboFlags_NoArrowButton))
						{
							for (int s = 0; s < IM_ARRAYSIZE(kSwizzles); ++s)
							{
								if (ImGui::Selectable(kSwizzles[s].name, s == current))
								{
									l.swizzle = kSwizzles[s].swizzle;
									l.width = kSwizzles[s].width;
									change |= kGraph;
								}
							}
							ImGui::EndCombo();
						}
						ImGui::PopID();
					}
					else if (current != 0)
					{
						const std::string text = "." + MaterialSwizzleText(l.swizzle) + (l.width ? "[w" + std::to_string(l.width) + "]" : "");
						ImGui::TextDisabled("%s", text.c_str());
					}
				}
				else if (k < 3)
				{
					// 연결하지 않은 핀: 값을 여기서 적는다 (셰이더에 숫자로 들어간다). UV 핀은 QuadUV
					const uint32_t pinWidth = MaterialNodePinWidth(n.type, k);
					ImGui::SameLine();
					if (pinWidth == 0)
						ImGui::TextDisabled("QuadUV");
					else
					{
						ImGui::PushID(static_cast<int>(k));
						change |= DragValue("##pin", &n.pinValues[k].x, pinWidth, editable);
						ImGui::PopID();
					}
				}
				ImNodes::EndInputAttribute();
			}

			change |= DrawNodeValues(n, editable);

			ImNodes::BeginOutputAttribute(OutPin(id));
			ImGui::Indent(110.f);
			ImGui::TextUnformatted("out");
			ImNodes::EndOutputAttribute();

			ImGui::PopID();
			ImNodes::EndNode();
			if (colors)
			{
				for (int c = 0; c < colors; ++c)
					ImNodes::PopColorStyle();
			}
		}

		for (int id = 0; id < count; ++id)
		{
			const MaterialNode& n = graph.nodes[id];
			for (uint32_t k = 0; k < n.inputs.size() && k < MaterialNodeInputCount(n.type); ++k)
			{
				const int src = n.inputs[k].node;
				if (src >= 0 && src < count)
					ImNodes::Link(InPin(id, k), OutPin(src), InPin(id, k));
			}
		}

		ImNodes::MiniMap(0.15f, ImNodesMiniMapLocation_BottomRight);
		if (editable)
			ImNodes::PopAttributeFlag();
		const bool canvasHovered = ImNodes::IsEditorHovered();
		ImNodes::EndNodeEditor();

		if (!editable)
		{
			ImGui::PopID();
			return change;
		}

		for (int id = 0; id < count; ++id)
		{
			const ImVec2 p = ImNodes::GetNodeGridSpacePos(id);
			MaterialNode& n = graph.nodes[id];
			if (p.x != n.pos.x || p.y != n.pos.y)
			{
				n.pos = { p.x, p.y };
				change |= kLayout;
			}
		}

		int startPin = 0, endPin = 0;
		if (ImNodes::IsLinkCreated(&startPin, &endPin))
		{
			if (!IsOutPin(startPin))
				std::swap(startPin, endPin);
			if (IsOutPin(startPin) && !IsOutPin(endPin))
			{
				const int src = startPin / kPinStride;
				const int dst = endPin / kPinStride;
				const uint32_t pin = static_cast<uint32_t>(endPin % kPinStride);
				if (src != dst && dst < count && src < count && pin < MaterialNodeInputCount(graph.nodes[dst].type))
				{
					MaterialNode& n = graph.nodes[dst];
					if (n.inputs.size() <= pin)
						n.inputs.resize(pin + 1);
					n.inputs[pin] = MaterialLink();
					n.inputs[pin].node = src;
					change |= kGraph;
				}
			}
		}

		int linkId = 0;
		if (ImNodes::IsLinkDestroyed(&linkId))
		{
			const int dst = linkId / kPinStride;
			const uint32_t pin = static_cast<uint32_t>(linkId % kPinStride);
			if (dst < count && pin < graph.nodes[dst].inputs.size())
			{
				graph.nodes[dst].inputs[pin].node = -1;
				TrimInputs(graph.nodes[dst]);
				change |= kGraph;
			}
		}

		const bool editorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (editorFocused && ImGui::IsKeyPressed(ImGuiKey_Delete))
		{
			const int links = ImNodes::NumSelectedLinks();
			if (links > 0)
			{
				std::vector<int> ids(links);
				ImNodes::GetSelectedLinks(ids.data());
				for (int l : ids)
				{
					const int dst = l / kPinStride;
					const uint32_t pin = static_cast<uint32_t>(l % kPinStride);
					if (dst < count && pin < graph.nodes[dst].inputs.size())
					{
						graph.nodes[dst].inputs[pin].node = -1;
						TrimInputs(graph.nodes[dst]);
					}
				}
				ImNodes::ClearLinkSelection();
				change |= kGraph;
			}
			const int nodes = ImNodes::NumSelectedNodes();
			if (nodes > 0)
			{
				std::vector<int> ids(nodes);
				ImNodes::GetSelectedNodes(ids.data());
				std::vector<bool> keep(count, true);
				for (int n : ids)
				{
					if (n >= 0 && n < count)
						keep[n] = false;
				}
				graph = KeepMaterialGraphNodes(graph, keep);
				for (MaterialNode& n : graph.nodes)
					TrimInputs(n);
				ImNodes::ClearNodeSelection();
				ImNodes::ClearLinkSelection();
				change |= kGraph;
			}
		}

		int hoveredNode = -1;
		const bool nodeHovered = ImNodes::IsNodeHovered(&hoveredNode);
		if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			if (nodeHovered)
			{
				ui.menuNode = hoveredNode;
				ImGui::OpenPopup("node-menu");
			}
			else
			{
				const ImVec2 mouse = ImGui::GetMousePos();
				const ImVec2 panning = ImNodes::EditorContextGetPanning();
				ui.addPos = ImVec2(mouse.x - canvasOrigin.x - panning.x, mouse.y - canvasOrigin.y - panning.y);
				ImGui::OpenPopup("add-node");
			}
		}

		if (ImGui::BeginPopup("add-node"))
		{
			ImGui::TextDisabled("Add node");
			ImGui::Separator();
			for (uint32_t t = 0; t < uint32_t(MaterialNodeType::Count); ++t)
			{
				if (!ImGui::MenuItem(MaterialNodeTypeName(MaterialNodeType(t))))
					continue;
				MaterialNode n = MakeMaterialNode(MaterialNodeType(t));
				if (n.type == MaterialNodeType::UVTransform)
					n.values[0] = { 1.f, 1.f, 0.f, 0.f };
				n.pos = { (std::max)(ui.addPos.x, 1.f), (std::max)(ui.addPos.y, 1.f) };
				graph.nodes.push_back(n);
				if (graph.output < 0)
					graph.output = static_cast<int>(graph.nodes.size()) - 1;
				change |= kGraph;
			}
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopup("node-menu"))
		{
			const int id = ui.menuNode;
			if (id >= 0 && id < static_cast<int>(graph.nodes.size()))
			{
				ImGui::TextDisabled("n%d %s", id, MaterialNodeTypeName(graph.nodes[id].type));
				ImGui::Separator();
				if (ImGui::MenuItem("Set as output", nullptr, id == graph.output))
				{
					graph.output = id;
					change |= kGraph;
				}
				if (ImGui::MenuItem("Delete"))
				{
					std::vector<bool> keep(graph.nodes.size(), true);
					keep[id] = false;
					graph = KeepMaterialGraphNodes(graph, keep);
					for (MaterialNode& n : graph.nodes)
						TrimInputs(n);
					ImNodes::ClearNodeSelection();
					ImNodes::ClearLinkSelection();
					change |= kGraph;
				}
			}
			ImGui::EndPopup();
		}

		ImGui::PopID();
		return change;
	}

	void Shutdown()
	{
		if (!s_ContextReady)
			return;
		for (auto& e : s_Editors)
		{
			if (e.second)
				ImNodes::EditorContextFree(e.second);
		}
		s_Editors.clear();
		s_Ui.clear();
		ImNodes::DestroyContext();
		s_ContextReady = false;
	}
}
