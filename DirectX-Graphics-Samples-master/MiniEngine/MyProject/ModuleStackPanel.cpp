#include "pch.h"
#include "ModuleStackPanel.h"

#include "EmitterAsset.h"
#include "EffectSystem.h"
#include "EmitterTestScene.h"
#include "EmitterRuntime.h"
#include "MaterialGraph.h"
#include "MaterialGraphEditor.h"
#include "ModuleLibrary.h"
#include "UberGpuProgram.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <cstring>

namespace
{
	const std::vector<ModuleDesc>& ModuleCatalog()
	{
		// 기본값은 MakeDefaultModule 한 곳에서 관리한다 (에셋 불러오기와 같은 값)
		static const std::vector<ModuleDesc> s_Catalog = []()
			{
				std::vector<ModuleDesc> catalog;
				for (uint32_t i = 0; i < uint32_t(ModuleId::Count); ++i)
					catalog.push_back(MakeDefaultModule(ModuleId(i)));
				return catalog;
			}();
		return s_Catalog;
	}

	// Uber / 생성 방식 상태 한 줄 + 공유 셰이더 데이터 문제 (ImGui 는 한글이 깨지므로 영어, 자세한 내용은 출력 창)
	void DrawSimStatus(const EmitterRuntime& runtime)
	{
		if (runtime.Backend() != EmitterBackend::Gpu)
			return;

		const UberEmitterData& uber = runtime.Uber();
		ImGui::Text("Sim: %s | generated shader: %s | shader compiles (total): %u",
			UberGpuProgram::ModeName(UberGpuProgram::Mode()),
			runtime.HasGeneratedProgram() ? "yes" : "no",
			EmitterGpuProgram::GetStats().lifetimeBuilds);

		if (!uber.errors.empty())
			ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f),
				"Uber: %u error(s) - this stack runs with the generated shader (see Output window)", uint32_t(uber.errors.size()));
		if (!uber.warnings.empty())
			ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
				"Uber: %u warning(s) - e.g. fixed execution order (see Output window)", uint32_t(uber.warnings.size()));
	}

	bool DrawFloats(const char* label, float* v, uint32_t count, const ParamDesc& p)
	{
		const float speed = (std::max)(std::fabs(v[0]) * 0.01f, 0.01f);

		bool changed = false;
		switch (count)
		{
		case 1: changed = ImGui::DragFloat(label, v, speed); break;
		case 2: changed = ImGui::DragFloat2(label, v, speed); break;
		case 3: changed = ImGui::DragFloat3(label, v, speed); break;
		case 4: changed = ImGui::DragFloat4(label, v, speed); break;
		default: break;
		}

		if (changed && p.uiHasMin)
			for (uint32_t i = 0; i < count; ++i)
				v[i] = (std::max)(v[i], p.uiMin);
		return changed;
	}

	bool DrawValue(const char* label, const ParamDesc& p, uint8_t* data)
	{
		if (p.type == &Types::Bool)
		{
			int32_t& stored = *reinterpret_cast<int32_t*>(data);
			bool enabled = stored != 0;
			if (!ImGui::Checkbox(label, &enabled))
				return false;
			stored = enabled ? 1 : 0;
			return true;
		}
		if (p.type == &Types::Int32 || p.type == &Types::UInt32)
			return ImGui::DragInt(label, reinterpret_cast<int*>(data));
		if (p.uiColor && p.type == &Types::Float4)
			return ImGui::ColorEdit4(label, reinterpret_cast<float*>(data), ImGuiColorEditFlags_Float);

		return DrawFloats(label, reinterpret_cast<float*>(data), p.type->size / sizeof(float), p);
	}

	// ---- 스프라이트 텍스처 ----
	const char* const kTextureDir = "Assets/Textures/";

	std::string LowerExtension(const std::string& name)
	{
		const size_t dot = name.find_last_of('.');
		std::string ext = (dot == std::string::npos) ? std::string() : name.substr(dot);
		for (char& c : ext)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return ext;
	}

	// 폴더의 이미지 파일 목록. .dds 는 변환 결과라서 원본(png 등)이 없을 때만 보여 준다
	std::vector<std::string> ListTextureFiles()
	{
		std::vector<std::string> sources, dds;

		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA((std::string(kTextureDir) + "*").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE)
			return sources;
		do
		{
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			const std::string name = fd.cFileName;
			const std::string ext = LowerExtension(name);
			if (ext == ".dds")
			{
				if (!ParticleTextures::IsCacheFile(name))
					dds.push_back(name);
			}
			else if (ext == ".png" || ext == ".tga" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp")
				sources.push_back(name);
		} while (FindNextFileA(h, &fd));
		FindClose(h);

		auto stem = [](const std::string& n) { return n.substr(0, n.find_last_of('.')); };
		for (const std::string& d : dds)
		{
			const bool hasSource = std::any_of(sources.begin(), sources.end(),
				[&](const std::string& s) { return stem(s) == stem(d); });
			if (!hasSource)
				sources.push_back(d);
		}

		std::sort(sources.begin(), sources.end());
		for (std::string& s : sources)
			s = kTextureDir + s;
		return sources;
	}


	void DrawTextureSlot(EmitterRuntime& runtime, uint32_t slot)
	{
		MaterialTexture t = runtime.Material().SlotTexture(slot);
		const char* emptyName = (slot == ParticleMaterial::kBaseTexture) ? "(soft circle)" : "(none = white)";
		const std::string preview = t.path.empty() ? std::string(emptyName) : t.path;
		const std::string label = std::string(ParticleMaterial::SlotName(slot)) + " Texture";

		ImGui::PushID(static_cast<int>(slot));
		bool changed = false;

		ImGui::SetNextItemWidth(300.f);
		if (ImGui::BeginCombo(label.c_str(), preview.c_str()))
		{
			if (ImGui::Selectable(emptyName, t.path.empty()))
			{
				t.path.clear();
				changed = true;
			}

			// 펼칠 때마다 다시 읽는다 (파일을 추가해도 재실행 필요 없음)
			for (const std::string& path : ListTextureFiles())
			{
				if (ImGui::Selectable(path.c_str(), path == t.path))
				{
					t.path = path;
					changed = true;
				}
			}
			ImGui::EndCombo();
		}

		// 색 텍스처 = sRGB, 노이즈 / 마스크처럼 값으로 쓰는 텍스처 = Linear
		ImGui::SameLine();
		static const char* const kColorSpaces[] = { "sRGB", "Linear" };
		int colorSpace = static_cast<int>(t.colorSpace);
		ImGui::SetNextItemWidth(90.f);
		if (ImGui::Combo("##ColorSpace", &colorSpace, kColorSpaces, IM_ARRAYSIZE(kColorSpaces)))
		{
			t.colorSpace = static_cast<TextureColorSpace>(colorSpace);
			changed = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("sRGB: color image\nLinear: data (noise, mask) - values are read as stored");

		if (changed)
			runtime.SetMaterialTexture(slot, t);

		if (slot >= ParticleMaterial::kTexture0 && runtime.Material().HasCustomGraph())
		{
			bool sampled = false;
			for (const MaterialNode& n : runtime.Material().CustomGraph()->nodes)
				sampled |= (n.type == MaterialNodeType::TextureSample && n.textureSlot == slot - ParticleMaterial::kTexture0);
			ImGui::SameLine();
			if (!sampled)
				ImGui::TextDisabled("(not used by graph)");
			else if (t.path.empty())
				ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "<- graph (empty: white)");
			else
				ImGui::TextDisabled("<- graph");
		}
		else

		// 이 슬롯을 어떤 효과가 쓰는가?
		if (slot >= ParticleMaterial::kTexture0)
		{
			const uint32_t effectSlot = slot - ParticleMaterial::kTexture0;
			const ParticleMaterial::LayerLayout* owner = nullptr;
			for (const ParticleMaterial::LayerLayout& l : runtime.Material().LayerLayouts())
			{
				if (effectSlot >= l.textureBase && effectSlot < l.textureBase + l.textureCount)
					owner = &l;
			}
			ImGui::SameLine();
			if (owner == nullptr)
				ImGui::TextDisabled("(no layer)");
			else
			{
				const char* use = ParticleMaterial::EffectTextureName(owner->effect, effectSlot - owner->textureBase);
				if (t.path.empty())		// 비어 있으면 흰색이라 그 층 효과가 티 없이 사라진다
					ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "<- %s %s (empty: white)", ParticleMaterial::EffectName(owner->effect), use ? use : "");
				else
					ImGui::TextDisabled("<- %s %s", ParticleMaterial::EffectName(owner->effect), use ? use : "");
			}
		}

		ImGui::PopID();
	}

	void DrawTexture(EmitterRuntime& runtime)
	{
		for (uint32_t slot = 0; slot < ParticleMaterial::kNumTextureSlot; ++slot)
			DrawTextureSlot(runtime, slot);
	}

	// ---- SubUV / 블렌드 ----
	void DrawSpriteSettings(EmitterRuntime& runtime)
	{
		SpriteSettings s = runtime.GetSpriteSettings();
		bool changed = false;

		int grid[2] = { static_cast<int>(s.subImageCols), static_cast<int>(s.subImageRows) };
		ImGui::SetNextItemWidth(160.f);
		if (ImGui::DragInt2("Sub Image Size", grid, 0.1f, 1, 64))
		{
			s.subImageCols = static_cast<uint32_t>((std::max)(grid[0], 1));
			s.subImageRows = static_cast<uint32_t>((std::max)(grid[1], 1));
			changed = true;
		}
		ImGui::SameLine();
		changed |= ImGui::Checkbox("Sub UV Blend", &s.subUVBlend);

		static const char* const kBlendModes[] = { "Translucent", "Additive" };
		int blend = static_cast<int>(s.blend);
		ImGui::SetNextItemWidth(160.f);
		if (ImGui::Combo("Blend", &blend, kBlendModes, IM_ARRAYSIZE(kBlendModes)))
		{
			s.blend = static_cast<SpriteBlendMode>(blend);
			changed = true;
		}

		// --- Texture의 ALpha 해석 방식 ---
		static const char* const kAlphaModes[] = { "Straight", "Premultiplied", "From Luminance" };
		int alphaMode = static_cast<int>(s.textureAlpha);
		ImGui::SetNextItemWidth(160.f);
		if (ImGui::Combo("Texture Alpha", &alphaMode, kAlphaModes, IM_ARRAYSIZE(kAlphaModes)))
		{
			s.textureAlpha = static_cast<SpriteTextureAlpha>(alphaMode);
			changed = true;
		}

		// ---- 빌보드 ----
		ImGui::SeparatorText("Billboard");

		auto enumCombo = [](const char* label, uint32_t& value, uint32_t count, const char* (*name)(uint32_t))
			{
				bool c = false;
				ImGui::SetNextItemWidth(220.f);
				if (ImGui::BeginCombo(label, name(value)))
				{
					for (uint32_t i = 0; i < count; ++i)
						if (ImGui::Selectable(name(i), i == value)) { value = i; c = true; }
					ImGui::EndCombo();
				}
				return c;
			};

		uint32_t facing = static_cast<uint32_t>(s.facing);
		if (enumCombo("Facing", facing, uint32_t(SpriteFacingMode::Count),
			[](uint32_t i) { return SpriteFacingName(SpriteFacingMode(i)); }))
		{
			s.facing = SpriteFacingMode(facing);
			changed = true;
		}
		uint32_t alignment = static_cast<uint32_t>(s.alignment);
		if (enumCombo("Alignment", alignment, uint32_t(SpriteAlignment::Count),
			[](uint32_t i) { return SpriteAlignmentName(SpriteAlignment(i)); }))
		{
			s.alignment = SpriteAlignment(alignment);
			changed = true;
		}

		// 자주 쓰는 조합 (Unity 의 Horizontal / Vertical Billboard)
		if (ImGui::SmallButton("Preset: Camera"))
		{
			s.facing = SpriteFacingMode::FaceCameraPlane; s.alignment = SpriteAlignment::Unaligned; changed = true;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Preset: Horizontal"))
		{
			s.facing = SpriteFacingMode::CustomFacingVector; s.alignment = SpriteAlignment::Unaligned; changed = true;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Preset: Vertical"))
		{
			s.facing = SpriteFacingMode::FaceCameraPosition; s.alignment = SpriteAlignment::CustomAlignment; changed = true;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Preset: Streak"))
		{
			s.facing = SpriteFacingMode::FaceCameraPosition; s.alignment = SpriteAlignment::VelocityAligned; changed = true;
		}
		if (s.facing == SpriteFacingMode::CustomFacingVector)
			ImGui::TextDisabled("Facing vector = 'SpriteFacing' binding (default (0,1,0) = lying flat)");
		if (s.alignment == SpriteAlignment::CustomAlignment)
			ImGui::TextDisabled("Up axis = 'SpriteAlignment' binding (default (0,1,0) = standing upright)");
		if (s.alignment == SpriteAlignment::VelocityAligned)
			ImGui::TextDisabled("Up axis = 'Velocity' binding (needs a module that writes Particles.Velocity)");

		float pivot[2] = { s.pivot.x, s.pivot.y };
		ImGui::SetNextItemWidth(160.f);
		if (ImGui::SliderFloat2("Pivot (uv)", pivot, 0.f, 1.f))
		{
			s.pivot = { pivot[0], pivot[1] };
			changed = true;
		}
		ImGui::SetNextItemWidth(160.f);
		changed |= ImGui::DragFloat("Camera Offset (m)", &s.cameraOffset, 0.01f, -10.f, 10.f);

		if (s.alignment == SpriteAlignment::VelocityAligned)
		{
			ImGui::SetNextItemWidth(160.f);
			changed |= ImGui::DragFloat("Velocity Stretch (s)", &s.velocityStretch, 0.001f, 0.f, 1.f);
		}
		if (s.facing == SpriteFacingMode::FaceCameraDistanceBlend)
		{
			float range[2] = { s.blendNear, s.blendFar };
			ImGui::SetNextItemWidth(160.f);
			if (ImGui::DragFloat2("Blend Near/Far (m)", range, 0.1f, 0.f, 1000.f))
			{
				s.blendNear = (std::max)(range[0], 0.f);
				s.blendFar = (std::max)(range[1], s.blendNear);
				changed = true;
			}
		}

		if (changed)
			runtime.SetSpriteSettings(s);
	}

	std::string LayerParamName(const char* name, uint32_t textureBase)
	{
		if (!name)
			return std::string();
		std::string out;
		for (const char* c = name; *c; ++c)
		{
			if (c[0] == 'T' && c[1] == 'e' && c[2] == 'x' && c[3] >= '0' && c[3] <= '9')
			{
				out += "Tex" + std::to_string(textureBase + uint32_t(c[3] - '0'));
				c += 3;
				continue;
			}
			out += *c;
		}

		return out;
	}

	bool DrawMaterialParam(EmitterRuntime& runtime, uint32_t k, const std::string& name)
	{
		float4 v = runtime.Material().Param(k);
		char label[96];
		if (!name.empty())
			snprintf(label, sizeof(label), "[%u] %s", k, name.c_str());
		else
			snprintf(label, sizeof(label), "[%u] Param", k);

		ImGui::SetNextItemWidth(300.f);
		if (!ImGui::DragFloat4(label, &v.x, 0.01f))
			return false;
		runtime.SetMaterialParam(k, v);
		return true;
	}

	bool s_ShowGraphEditor = false;

	void DrawMaterialGraphWindow(EmitterRuntime& runtime)
	{
		ImGui::SetNextWindowSize(ImVec2(960.f, 560.f), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Material Graph", &s_ShowGraphEditor))
		{
			runtime.EndMaterialLiveEdit();
			ImGui::End();
			return;
		}

		const ParticleMaterial& material = runtime.Material();
		const bool editable = material.HasCustomGraph();
		MaterialGraph graph = material.CurrentGraph();

		if (editable)
		{
			if (ImGui::Button("Auto layout"))
			{
				LayoutMaterialGraph(graph);
				runtime.SetMaterialGraphLayout(graph);
			}
			ImGui::SameLine();
			if (ImGui::Button("Back to preset"))
			{
				runtime.ClearMaterialGraph();
				ImGui::End();
				return;
			}
		}
		else if (ImGui::Button("Edit as graph"))
		{
			runtime.SetMaterialGraph(material.LayersAsGraph());
			ImGui::End();
			return;
		}
		ImGui::SameLine();
		const std::string& error = material.ProgramError();
		if (!error.empty())
			ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "drawing with the last good program");

		const MaterialCompileResult compiled = CompileMaterialGraph(graph);
		const std::string editorId = std::string(editable ? "edit#" : "view#") + std::to_string(reinterpret_cast<uintptr_t>(&runtime));
		const uint32_t change = MaterialGraphEditor::Draw(editorId.c_str(), graph, compiled, editable);
		// kGraph = 구성이 바뀌었거나 값을 놓음 -> 셰이더를 다시 만든다
		// kValue = 값을 드래그하는 중 -> 값만 넣고 인터프리터로 미리 본다 (놓으면 kGraph 가 온다)
		if (editable && (change & MaterialGraphEditor::kGraph))
			runtime.SetMaterialGraph(graph);
		else if (editable && (change & MaterialGraphEditor::kValue))
			runtime.SetMaterialGraph(graph, true);
		else if (runtime.MaterialLiveEdit() && !ImGui::IsAnyItemActive())
			runtime.EndMaterialLiveEdit();		// 놓은 것을 못 받은 경우 (드래그 중에 창이 가려지는 등)

		if (editable && (change & MaterialGraphEditor::kLayout))
			runtime.SetMaterialGraphLayout(graph);

		ImGui::End();
	}

	void DrawMaterialParams(EmitterRuntime& runtime)
	{
		if (!ImGui::TreeNode("Material Params"))
			return;

		{
			using Effect = ParticleMaterial::Effect;
			const uint32_t numEffects = uint32_t(Effect::Count) - 1;		// None 은 층으로 넣지 않는다 (층 없음 = None)
			const char* names[uint32_t(Effect::Count) - 1];
			for (uint32_t i = 0; i < numEffects; ++i)
				names[i] = ParticleMaterial::EffectName(Effect(i + 1));

			const ParticleMaterial& material = runtime.Material();

			// ---- 모드: Preset (효과 하나, 고정 셰이더) / Custom (그래프, 생성된 셰이더) ----
			const bool custom = material.GetMode() == ParticleMaterial::Mode::Custom;
			ImGui::TextUnformatted("Mode:");
			ImGui::SameLine();
			if (ImGui::RadioButton("Preset", !custom) && custom)
				runtime.ClearMaterialGraph();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("One effect, drawn with that effect's own fixed pixel shader.\n"
					"Switching back from Custom discards the graph (params reset to the preset defaults).");
			ImGui::SameLine();
			if (ImGui::RadioButton("Custom", custom) && !custom)
			{
				// 지금 Preset 을 그대로 그래프로 옮겨서 시작한다
				runtime.SetMaterialGraph(material.LayersAsGraph());
				s_ShowGraphEditor = true;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Node graph, drawn with a pixel shader generated for that graph.\n"
					"Starts from the current preset as a graph.");
			if (custom)
			{
				ImGui::SameLine();
				ImGui::Checkbox("Graph Editor", &s_ShowGraphEditor);
			}

			if (!custom)
			{
				// Preset: 효과 하나를 고른다 (None 포함)
				const char* presetNames[uint32_t(Effect::Count)];
				for (uint32_t i = 0; i < uint32_t(Effect::Count); ++i)
					presetNames[i] = ParticleMaterial::EffectName(Effect(i));

				int preset = static_cast<int>(material.GetEffect());
				ImGui::SetNextItemWidth(160.f);
				if (ImGui::Combo("Preset", &preset, presetNames, static_cast<int>(Effect::Count)))
					runtime.SetMaterialEffect(Effect(preset));
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Pattern: color x Tex.r / Tint: color x Tex.rgb / Dissolve: Tex = mask, progress = NormalizedAge\n"
						"Distortion: shifts the base texture UV / Glow: Tex x Tex -> add color");

				// 그 효과가 쓰는 입력값 / 텍스처 칸
				for (const ParticleMaterial::LayerLayout& l : material.LayerLayouts())
				{
					ImGui::SameLine();
					if (l.textureCount == 0)
						ImGui::TextDisabled("params [%u ~ %u]", l.paramBase, l.paramBase + l.paramCount - 1);
					else if (l.textureCount == 1)
						ImGui::TextDisabled("params [%u ~ %u]  Tex%u", l.paramBase, l.paramBase + l.paramCount - 1, l.textureBase);
					else
						ImGui::TextDisabled("params [%u ~ %u]  Tex%u, Tex%u", l.paramBase, l.paramBase + l.paramCount - 1, l.textureBase, l.textureBase + 1);

					for (uint32_t t = 0; t < l.textureCount; ++t)
					{
						if (!material.SlotTexture(ParticleMaterial::kTexture0 + l.textureBase + t).path.empty())
							continue;
						ImGui::SameLine();
						ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "Tex%u empty", l.textureBase + t);
					}
				}

				// 효과를 하나 더 섞기: Preset 은 효과 하나까지라서, 섞은 결과는 그래프(Custom) 가 된다
				if (material.GetEffect() != Effect::None)
				{
					static int s_AddEffect = 0;		// 섞을 효과 (UI 상태만)
					ImGui::SetNextItemWidth(160.f);
					ImGui::Combo("##addEffect", &s_AddEffect, names, static_cast<int>(numEffects));
					ImGui::SameLine();
					if (ImGui::Button("Combine -> Custom"))
					{
						if (runtime.AddMaterialLayer(Effect(s_AddEffect + 1)))
							s_ShowGraphEditor = true;
					}
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Stack another effect on top of this preset.\n"
							"A preset is one effect, so the combined result becomes a Custom graph.");
				}
				if (!material.LayerError().empty())
					ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "%s", material.LayerError().c_str());
			}

			// ---- 지금 그리는 픽셀 셰이더 (Interpreted 는 Main 창의 "Material mode" 에서 고른다) ----
			switch (runtime.MaterialShader())
			{
			case EmitterRuntime::MaterialShaderKind::Preset:
				ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.f), "Pixel shader: preset (%s)", SpriteRenderer::PresetShaderName(material.GetEffect()));
				break;
			case EmitterRuntime::MaterialShaderKind::Generated:
				ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.f), "Pixel shader: generated for this graph");
				if (ImGui::TreeNode("Generated PS Source"))
				{
					ImGui::TextUnformatted(runtime.GeneratedMaterialSource().c_str());
					ImGui::TreePop();
				}
				break;
			default:
				if (runtime.MaterialLiveEdit())
					ImGui::TextDisabled("Pixel shader: interpreter (editing a value; generated on release)");
				else if (MaterialGpuProgram::Mode() == MaterialShaderMode::Generated)
					ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "Pixel shader: interpreter (generated build failed)");
				else
					ImGui::TextDisabled("Pixel shader: interpreter (Material mode = Interpreted)");
				break;
			}
			{
				const std::string& genError = runtime.GeneratedMaterialError();
				if (!genError.empty())
					ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "%s", genError.c_str());
			}

			// 아래 설정은 Interpreted 방식과 공용 렌더 목록에만 쓰인다
			bool useVM = runtime.Material().UseVM();
			if (ImGui::Checkbox("Run as VM", &useVM))
				runtime.SetMaterialUseVM(useVM);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Draw the same effect with the material interpreter (MaterialVM.hlsli).\nThe image must not change when toggled.");
			if (runtime.Material().Layers().size() > 1)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("(2+ layers: always VM + Graph compile)");
			}

			{
				static const char* const kSources[] = { "Hand list", "Graph compile" };
				int source = static_cast<int>(runtime.Material().GetProgramSource());
				ImGui::SetNextItemWidth(160.f);
				if (ImGui::Combo("VM Program Source", &source, kSources, IM_ARRAYSIZE(kSources)))
					runtime.SetMaterialProgramSource(static_cast<ParticleMaterial::ProgramSource>(source));
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Hand list: hand-written instructions\nGraph compile: built from the effect graph by CompileMaterialGraph\nThe image must not change.");

				const std::string& error = runtime.Material().ProgramError();
				if (!error.empty())
					ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "%s", error.c_str());

				if (ImGui::TreeNode("Effect Graph"))
				{
					const MaterialGraph graph = runtime.Material().CurrentGraph();
					const MaterialCompileResult compiled = CompileMaterialGraph(graph);
					const std::string text = DumpMaterialGraph(graph, &compiled);
					ImGui::TextUnformatted(text.c_str());
					ImGui::TreePop();
				}
			}

			const MaterialProgram& program = runtime.Material().Program();

			uint32_t regs = 0;
			for (const MaterialInstr& instr : program.instrs)
				regs = (std::max)(regs, instr.dst + 1);
			if (ImGui::TreeNode("VM Program", "VM Program (%u / %u instrs, %u / %u regs)", uint32_t(program.instrs.size()),
				MaterialProgram::kMaxInstrs, regs, MaterialProgram::kMaxRegs))
			{
				const std::string text = DumpMaterialProgram(program);
				ImGui::TextUnformatted(text.c_str());
				ImGui::TreePop();
			}
		}

		if (runtime.Material().HasCustomGraph())
		{
			ImGui::TextDisabled("Values are edited on the nodes and their unlinked pins (Material Graph window).\n"
				"They are written into the generated shader as constants.");
			ImGui::TreePop();
			return;
		}

		const std::vector<ParticleMaterial::LayerLayout>& layouts = runtime.Material().LayerLayouts();
		uint32_t used = 0;
		for (const ParticleMaterial::LayerLayout& l : layouts)
		{
			ImGui::TextDisabled("%s", ParticleMaterial::EffectName(l.effect));
			for (uint32_t k = 0; k < l.paramCount; ++k)
				DrawMaterialParam(runtime, l.paramBase + k, LayerParamName(ParticleMaterial::ParamName(k, l.effect), l.textureBase));
			used = l.paramBase + l.paramCount;
		}

		if (ImGui::TreeNode("Unused", "Unused [%u ~ %u]", used, ParticleMaterial::kMaxParams - 1))
		{
			for (uint32_t k = used; k < ParticleMaterial::kMaxParams; ++k)
				DrawMaterialParam(runtime, k, std::string());
			ImGui::TreePop();
		}

		if (ImGui::SmallButton("Reset Params"))
		{
			for (uint32_t k = 0; k < ParticleMaterial::kMaxParams; ++k)
				runtime.SetMaterialParam(k, ParticleMaterial::DefaultParam(k, ParticleMaterial::Effect::None));
			for (const ParticleMaterial::LayerLayout& l : layouts)
			{
				for (uint32_t k = 0; k < l.paramCount; ++k)
					runtime.SetMaterialParam(l.paramBase + k, ParticleMaterial::DefaultParam(k, l.effect));
			}
		}
		ImGui::TreePop();
	}

	// 렌더러: 읽는 속성은 고정 (렌더 구조체). 칸마다 "아무 모듈도 그 속성을 쓰지 않을 때의 값" 만 고친다
	void DrawBindings(EmitterRuntime& runtime)
	{
		const RendererDesc& desc = runtime.SpriteBindings();
		ImGui::SeparatorText(desc.name.c_str());

		DrawTexture(runtime);
		DrawMaterialParams(runtime);
		DrawSpriteSettings(runtime);

		ImGui::TextDisabled("Defaults (used when no module writes the attribute; applies to newly spawned particles)");
		for (size_t i = 0; i < desc.bindings.size(); ++i)
		{
			const RenderBinding& b = desc.bindings[i];
			ImGui::PushID(static_cast<int>(i));

			ParamDesc view;                      // DrawValue 를 재사용하려고 모양만 맞춘다
			view.name = b.slot;
			view.type = b.type;
			view.uiColor = (b.type == &Types::Float4);
			std::vector<uint8_t> value = b.defaultValue;

			ImGui::SetNextItemWidth(220.f);
			if (DrawValue(b.slot.c_str(), view, value.data()))
				runtime.SetBindingDefault(i, value);

			ImGui::PopID();
		}
	}

	// 커브 편집: 키 표 (시간 + 값) + 구운 결과 미리보기
	//   바뀌면 그 커브만 다시 굽는다. 전역 커브 버퍼의 칸은 그대로라 재컴파일 / 이미터 재시작이 없다
	void DrawCurve(ParamDesc& p)
	{
		CurveAsset& curve = *p.curve;
		std::vector<CurveKey>& keys = curve.Keys();
		const uint32_t count = (std::min)(uint32_t(p.type->size / sizeof(float)), 4u);

		bool changed = false;
		int removeKey = -1;

		ImGui::Indent();
		ImGui::TextDisabled("x = %s (0..1), linear between keys", p.curveIndex.c_str());

		for (size_t k = 0; k < keys.size(); ++k)
		{
			ImGui::PushID(static_cast<int>(k));

			ImGui::SetNextItemWidth(80.f);
			if (ImGui::DragFloat("##time", &keys[k].time, 0.005f, 0.f, 1.f, "t %.3f"))
			{
				keys[k].time = (std::min)((std::max)(keys[k].time, 0.f), 1.f);
				changed = true;
			}

			ImGui::SameLine();
			ImGui::SetNextItemWidth(220.f);
			if (p.uiColor && count == 4)
				changed |= ImGui::ColorEdit4("##value", &keys[k].value.x, ImGuiColorEditFlags_Float);
			else
				changed |= DrawFloats("##value", &keys[k].value.x, count, p);

			ImGui::SameLine();
			ImGui::BeginDisabled(keys.size() <= 1);
			if (ImGui::SmallButton("-"))
				removeKey = static_cast<int>(k);
			ImGui::EndDisabled();

			ImGui::PopID();
		}

		if (removeKey >= 0)
		{
			keys.erase(keys.begin() + removeKey);
			changed = true;
		}

		if (ImGui::SmallButton("+ key"))
		{
			// 가운데에 지금 커브 값으로 추가 (추가한 직후에는 결과가 그대로)
			CurveKey key;
			key.time = 0.5f;
			key.value = curve.Evaluate(0.5f);
			keys.push_back(key);
			changed = true;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Sort keys"))
			curve.SortKeys();

		if (changed)
			curve.Bake();

		// 미리보기: 셰이더가 실제로 읽는 표 (성분마다 한 줄)
		static const char* const kComponent[] = { "x", "y", "z", "w" };
		const float4* lut = CurveLibrary::Samples(curve.Slot());
		for (uint32_t c = 0; c < count; ++c)
		{
			float samples[CurveLibrary::kSamples];
			for (uint32_t i = 0; i < CurveLibrary::kSamples; ++i)
				samples[i] = (&lut[i].x)[c];

			ImGui::PushID(static_cast<int>(c));
			ImGui::PlotLines("##plot", samples, static_cast<int>(CurveLibrary::kSamples), 0,
				count > 1 ? kComponent[c] : nullptr, FLT_MAX, FLT_MAX, ImVec2(300.f, 36.f));
			ImGui::PopID();
		}

		if (curve.Slot() == CurveLibrary::kIdentitySlot)
			ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "No free curve slot - this curve is fixed to 1");

		ImGui::Unindent();
	}

	void DrawParam(EmitterRuntime& runtime, size_t moduleIndex, size_t paramIndex)
	{
		ModuleDesc& module = runtime.EditModules()[moduleIndex];
		ParamDesc& p = module.params[paramIndex];

		ImGui::PushID(static_cast<int>(paramIndex));
		ImGui::TextUnformatted(p.name.c_str());

		// 단계마다 고를 수 있는 종류가 다르다: Spawn = 파티클별 랜덤 / Update = 수명에 따른 커브
		const bool canRandom = module.stage == Stage::ParticleSpawn && IsFloatFamily(p.type);
		const bool canCurve = module.stage == Stage::ParticleUpdate && IsFloatFamily(p.type);
		if (canRandom || canCurve)
		{
			const ParamKind other = canRandom ? ParamKind::RandomRange : ParamKind::Curve;
			const char* const kinds[] = { "Constant", canRandom ? "Random Range" : "Curve" };
			int kind = (p.kind == ParamKind::Constant) ? 0 : 1;

			ImGui::SameLine();
			ImGui::SetNextItemWidth(130.f);
			if (ImGui::Combo("##kind", &kind, kinds, IM_ARRAYSIZE(kinds)))
				runtime.SetParamKind(moduleIndex, paramIndex, kind == 0 ? ParamKind::Constant : other);
		}

		bool changed = false;
		if (p.kind == ParamKind::Constant)
		{
			changed = DrawValue("##value", p, p.value.data());
		}
		else if (p.kind == ParamKind::Curve)
		{
			if (p.curve)
				DrawCurve(p);   // 다시 굽는 것으로 끝 (NotifyValuesChanged 필요 없음)
		}
		else
		{
			changed |= DrawValue("Min", p, p.value.data());
			changed |= DrawValue("Max", p, p.value.data() + p.type->size);

			if (p.type->size > sizeof(float))
			{
				bool perComponent = p.perComponent;
				if (ImGui::Checkbox("Per Component", &perComponent))
					runtime.SetPerComponent(moduleIndex, paramIndex, perComponent);
			}
		}

		if (changed)
			runtime.NotifyValuesChanged();
		ImGui::PopID();
	}

	// ---- 에셋 저장 / 불러오기 (Assets/Emitters/*.emitter.json) ----
	//   ImGui 는 한글이 깨지므로 문구는 영어. 자세한 내용은 출력 창에도 남긴다
	struct AssetPanelState
	{
		char                     name[65] = "NewEmitter";
		std::string              status;
		bool                     statusIsError = false;
		std::vector<std::string> warnings;
	};

	AssetPanelState& AssetState()
	{
		static AssetPanelState s_State;
		return s_State;
	}

	void SetAssetStatus(const std::string& status, bool isError)
	{
		AssetPanelState& st = AssetState();
		st.status = status;
		st.statusIsError = isError;
		Utility::Printf("[EmitterAsset] %s\n", status.c_str());
		for (const std::string& w : st.warnings)
			Utility::Printf("[EmitterAsset]   warning: %s\n", w.c_str());
	}

	void SaveAsset(EmitterRuntime& runtime, const std::string& name)
	{
		AssetPanelState& st = AssetState();
		st.warnings.clear();

		const std::string path = EmitterAssetPath(name);
		std::string error;
		if (SaveEmitterAsset(path, CaptureEmitterAsset(runtime, name), error))
			SetAssetStatus("Saved " + path, false);
		else
			SetAssetStatus("Save failed: " + error, true);
	}

	void LoadAsset(EmitterRuntime& runtime, const std::string& name)
	{
		AssetPanelState& st = AssetState();
		st.warnings.clear();

		const std::string path = EmitterAssetPath(name);
		EmitterAsset asset;
		std::string error;
		if (!LoadEmitterAsset(path, asset, error, st.warnings))
		{
			SetAssetStatus("Load failed: " + error, true);   // 지금 이미터는 그대로
			return;
		}

		std::vector<std::string> failedTextures;
		if (!ApplyEmitterAsset(runtime, asset, &failedTextures))
		{
			for (const std::string& p : failedTextures)
				st.warnings.push_back("texture '" + p + "' could not be loaded (drawn magenta)");
		}
		strncpy_s(st.name, sizeof(st.name), name.c_str(), _TRUNCATE);
		SetAssetStatus("Loaded " + path + " (restarting emitter)", false);
	}

	void DrawAssetPanel(EmitterRuntime& runtime)
	{
		AssetPanelState& st = AssetState();
		ImGui::SeparatorText("Emitter Asset");

		ImGui::SetNextItemWidth(200.f);
		ImGui::InputText("##assetName", st.name, sizeof(st.name));
		const std::string name = st.name;

		ImGui::SameLine();
		if (ImGui::Button("Save"))
		{
			if (!IsValidEmitterAssetName(name))
			{
				st.warnings.clear();
				SetAssetStatus("Invalid name (use A-Z a-z 0-9 _ - .)", true);
			}
			else if (GetFileAttributesA(EmitterAssetPath(name).c_str()) != INVALID_FILE_ATTRIBUTES)
				ImGui::OpenPopup("Overwrite asset?");
			else
				SaveAsset(runtime, name);
		}

		if (ImGui::BeginPopupModal("Overwrite asset?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("%s already exists. Overwrite?", EmitterAssetPath(name).c_str());
			if (ImGui::Button("Overwrite"))
			{
				SaveAsset(runtime, name);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		ImGui::SameLine();
		ImGui::SetNextItemWidth(200.f);
		if (ImGui::BeginCombo("Load", "Select..."))
		{
			// 펼칠 때마다 다시 읽는다 (파일을 추가해도 재실행 필요 없음)
			const std::vector<std::string> assets = ListEmitterAssets();
			if (assets.empty())
				ImGui::TextDisabled("(no files in %s)", EmitterAssetDirectory());
			for (const std::string& asset : assets)
				if (ImGui::Selectable(asset.c_str()))
					LoadAsset(runtime, asset);
			ImGui::EndCombo();
		}

		if (!st.status.empty())
		{
			const ImVec4 color = st.statusIsError ? ImVec4(1.f, 0.3f, 0.3f, 1.f) : ImVec4(0.5f, 1.f, 0.5f, 1.f);
			ImGui::TextColored(color, "%s", st.status.c_str());
		}
		for (const std::string& w : st.warnings)
			ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f), "%s", w.c_str());
	}
}

void DrawModuleStackPanel(EmitterRuntime& runtime)
{
	DrawAssetPanel(runtime);

	ImGui::SeparatorText("Backend");

	const EmitterBackend backend = runtime.Backend();
	if (ImGui::RadioButton("CPU", backend == EmitterBackend::Cpu))
		runtime.SetBackend(EmitterBackend::Cpu);
	ImGui::SameLine();
	if (ImGui::RadioButton("GPU", backend == EmitterBackend::Gpu))
		runtime.SetBackend(EmitterBackend::Gpu);
	ImGui::SameLine();
	if (ImGui::Button("Restart"))
		runtime.Restart();

	if (runtime.Backend() == EmitterBackend::Gpu)
		ImGui::Text("Alive : %u / %u  (readback, 3 frames late)", runtime.AliveCount(), runtime.Capacity());
	else
		ImGui::Text("Alive : %u / %u", runtime.AliveCount(), runtime.Capacity());

	if (!runtime.LastError().empty())
		ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "%s", runtime.LastError().c_str());
	for (const std::string& warning : runtime.Warnings())
		ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f), "%s", warning.c_str());

	DrawSimStatus(runtime);

	// 순서 변경은 목록을 다 그린 뒤 적용한다
	int moveIndex = -1;
	int moveDirection = 0;
	int removeIndex = -1;

	static const Stage kStages[] = { Stage::EmitterUpdate, Stage::ParticleSpawn, Stage::ParticleUpdate };
	for (Stage stage : kStages)
	{
		ImGui::SeparatorText(StageName(stage));

		std::vector<ModuleDesc>& modules = runtime.EditModules();
		for (size_t i = 0; i < modules.size(); ++i)
		{
			if (modules[i].stage != stage)
				continue;

			ImGui::PushID(static_cast<int>(i));

			ImGui::BeginDisabled(!runtime.CanMoveModule(i, -1));
			if (ImGui::ArrowButton("##up", ImGuiDir_Up))
			{
				moveIndex = static_cast<int>(i);
				moveDirection = -1;
			}
			ImGui::EndDisabled();

			ImGui::SameLine();
			ImGui::BeginDisabled(!runtime.CanMoveModule(i, 1));
			if (ImGui::ArrowButton("##down", ImGuiDir_Down))
			{
				moveIndex = static_cast<int>(i);
				moveDirection = 1;
			}
			ImGui::EndDisabled();

			ImGui::SameLine();
			if (ImGui::SmallButton("X"))
				removeIndex = static_cast<int>(i);

			ImGui::SameLine();
			if (ImGui::TreeNodeEx(modules[i].name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
			{
				for (size_t p = 0; p < modules[i].params.size(); ++p)
					DrawParam(runtime, i, p);
				ImGui::TreePop();
			}

			ImGui::PopID();
		}
	}

	if (moveIndex >= 0)
		runtime.MoveModule(static_cast<size_t>(moveIndex), moveDirection);

	if (removeIndex >= 0)
		runtime.RemoveModule(static_cast<size_t>(removeIndex));

	ImGui::Spacing();
	if (ImGui::BeginCombo("Add module", "Select..."))
	{
		const std::vector<ModuleDesc>& catalog = ModuleCatalog();
		for (size_t c = 0; c < catalog.size(); ++c)
		{
			if (ImGui::Selectable(catalog[c].name.c_str()))
				runtime.AddModule(catalog[c]);
		}
		ImGui::EndCombo();
	}

	DrawBindings(runtime);

	if (s_ShowGraphEditor)
		DrawMaterialGraphWindow(runtime);
	else
		runtime.EndMaterialLiveEdit();
}

namespace
{
	struct SystemPanelState
	{
		char                     name[65] = "NewSystem";
		std::string              status;
		bool                     statusIsError = false;
		std::vector<std::string> warnings;
		const EmitterRuntime*    owner = nullptr;
		char                     rename[65] = "";
		float3                   eulerDeg = { 0.f, 0.f, 0.f };
	};

	SystemPanelState& SystemState()
	{
		static SystemPanelState s_State;
		return s_State;
	}

	void SetSystemStatus(const std::string& status, bool isError)
	{
		SystemPanelState& st = SystemState();
		st.status = status;
		st.statusIsError = isError;
	}

	std::vector<ModuleDesc> DefaultEmitterModules()
	{
		return MakePreset(EmitterPreset::Basic, 50.f);
	}

	void SaveSystem(EffectSystem& system, const std::string& name)
	{
		SystemPanelState& st = SystemState();
		st.warnings.clear();
		std::string error;
		const std::string path = SystemAssetPath(name);
		if (SaveSystemAsset(path, system.Capture(name), error))
			SetSystemStatus("Saved " + path + " (" + std::to_string(system.Count()) + " emitters)", false);
		else
			SetSystemStatus("Save failed: " + error, true);
	}

	void LoadSystem(EffectSystem& system, int& selected, const std::string& name)
	{
		SystemPanelState& st = SystemState();
		st.warnings.clear();
		SystemAsset asset;
		std::string error;
		if (!LoadSystemAsset(SystemAssetPath(name), asset, error, st.warnings))
		{
			SetSystemStatus("Load failed: " + error, true);
			return;
		}
		std::vector<std::string> failures;
		system.Load(asset, &failures);
		for (const std::string& f : failures)
			st.warnings.push_back(f);
		selected = system.Count() > 0 ? 0 : -1;
		snprintf(st.name, sizeof(st.name), "%s", name.c_str());
		SetSystemStatus("Loaded " + name + " (" + std::to_string(system.Count()) + " emitters)", false);
	}
}

void DrawSystemPanel(EffectSystem& system, int& selected)
{
	SystemPanelState& st = SystemState();
	ImGui::SeparatorText("Effect System");

	ImGui::SetNextItemWidth(200.f);
	ImGui::InputText("##systemName", st.name, sizeof(st.name));
	const std::string name = st.name;
	ImGui::SameLine();
	if (ImGui::Button("Save##system"))
	{
		if (!IsValidEmitterAssetName(name))
			SetSystemStatus("Invalid name (use A-Z a-z 0-9 _ - .)", true);
		else if (GetFileAttributesA(SystemAssetPath(name).c_str()) != INVALID_FILE_ATTRIBUTES)
			ImGui::OpenPopup("Overwrite system?");
		else
			SaveSystem(system, name);
	}
	if (ImGui::BeginPopupModal("Overwrite system?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("%s already exists. Overwrite?", SystemAssetPath(name).c_str());
		if (ImGui::Button("Overwrite"))
		{
			SaveSystem(system, name);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(160.f);
	if (ImGui::BeginCombo("##loadSystem", "Load..."))
	{
		const std::vector<std::string> systems = ListSystemAssets();
		if (systems.empty())
			ImGui::TextDisabled("(no files in %s)", SystemAssetDirectory());
		for (const std::string& s : systems)
			if (ImGui::Selectable(s.c_str()))
				LoadSystem(system, selected, s);
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::Button("New##system"))
	{
		system.Clear();
		selected = system.AddEmitter("Emitter", DefaultEmitterModules(), 10'000, EmitterBackend::Gpu);
		snprintf(st.name, sizeof(st.name), "NewSystem");
		SetSystemStatus("New system", false);
	}

	if (!st.status.empty())
	{
		const ImVec4 color = st.statusIsError ? ImVec4(1.f, 0.3f, 0.3f, 1.f) : ImVec4(0.5f, 1.f, 0.5f, 1.f);
		ImGui::TextColored(color, "%s", st.status.c_str());
	}
	for (const std::string& w : st.warnings)
		ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f), "%s", w.c_str());

	ImGui::Text("Alive : %u / %u  (%u emitters)", system.AliveTotal(), system.CapacityTotal(), uint32_t(system.Count()));
	if (system.Count() > 1)
	{
		bool sortTogether = system.GlobalSort();
		if (ImGui::Checkbox("Sort emitters together", &sortTogether))
			system.SetGlobalSort(sortTogether);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("On: all emitters' particles are depth-sorted in one list and drawn in one draw call\n(smoke in front of fire stays correct between emitters). Materials run on the interpreter.\nOff: each emitter sorts and draws on its own, with its own shader.\nEmitters are ordered by sort key (larger = on top), then far to near by emitter position.");
	}

	int moveIndex = -1, moveDirection = 0, removeIndex = -1, duplicateIndex = -1;
	for (size_t i = 0; i < system.Count(); ++i)
	{
		EffectSystem::Emitter& e = system.At(i);
		ImGui::PushID(static_cast<int>(i));
		ImGui::Checkbox("##enabled", &e.enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Off: this emitter stops (no simulation, not drawn)");
		ImGui::SameLine();
		if (ImGui::Selectable(e.name.c_str(), selected == static_cast<int>(i), 0, ImVec2(160.f, 0.f)))
			selected = static_cast<int>(i);
		ImGui::SameLine();
		ImGui::BeginDisabled(i == 0);
		if (ImGui::ArrowButton("##up", ImGuiDir_Up)) { moveIndex = static_cast<int>(i); moveDirection = -1; }
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(i + 1 == system.Count());
		if (ImGui::ArrowButton("##down", ImGuiDir_Down)) { moveIndex = static_cast<int>(i); moveDirection = 1; }
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::SmallButton("Dup"))
			duplicateIndex = static_cast<int>(i);
		ImGui::SameLine();
		if (ImGui::SmallButton("X"))
			removeIndex = static_cast<int>(i);
		// 준 List 방식의 정렬 키: 클수록 나중에 = 더 위에 그려진다 (같으면 카메라에서 먼 이미터부터)
		ImGui::SameLine();
		ImGui::SetNextItemWidth(70.f);
		ImGui::InputInt("##sortKey", &e.sortKey, 0);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Sort key: emitters with a larger key are drawn later (on top).\nSame key: far to near by emitter position.\nUsed when \"Sort emitters together\" is off.");
		ImGui::PopID();
	}

	if (duplicateIndex >= 0)
	{
		const int added = system.DuplicateEmitter(static_cast<size_t>(duplicateIndex));
		if (added >= 0)
			selected = added;
		else
			SetSystemStatus(system.LastError(), true);
	}
	else if (moveIndex >= 0)
	{
		system.MoveEmitter(static_cast<size_t>(moveIndex), moveDirection);
		if (selected == moveIndex)
			selected += moveDirection;
		else if (selected == moveIndex + moveDirection)
			selected -= moveDirection;
	}
	else if (removeIndex >= 0)
	{
		system.RemoveEmitter(static_cast<size_t>(removeIndex));
		if (selected >= static_cast<int>(system.Count()))
			selected = static_cast<int>(system.Count()) - 1;
	}

	if (ImGui::Button("Add emitter"))
	{
		const int added = system.AddEmitter("Emitter " + std::to_string(system.Count()), DefaultEmitterModules(), 10'000, EmitterBackend::Gpu);
		if (added >= 0)
			selected = added;
		else
			SetSystemStatus(system.LastError(), true);
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(200.f);
	if (ImGui::BeginCombo("##addFromAsset", "Add from emitter asset..."))
	{
		const std::vector<std::string> assets = ListEmitterAssets();
		if (assets.empty())
			ImGui::TextDisabled("(no files in %s)", EmitterAssetDirectory());
		for (const std::string& asset : assets)
		{
			if (!ImGui::Selectable(asset.c_str()))
				continue;
			EmitterAsset a;
			std::string error;
			st.warnings.clear();
			if (!LoadEmitterAsset(EmitterAssetPath(asset), a, error, st.warnings))
			{
				SetSystemStatus("Load failed: " + error, true);
				continue;
			}
			const int added = system.AddEmitter(asset, a);
			if (added >= 0)
			{
				selected = added;
				SetSystemStatus("Added " + asset, false);
			}
			else
				SetSystemStatus(system.LastError(), true);
		}
		ImGui::EndCombo();
	}

	if (selected < 0 || selected >= static_cast<int>(system.Count()))
	{
		selected = system.Count() > 0 ? 0 : -1;
		return;
	}
	EffectSystem::Emitter& e = system.At(static_cast<size_t>(selected));
	if (st.owner != e.runtime.get())
	{
		st.owner = e.runtime.get();
		snprintf(st.rename, sizeof(st.rename), "%s", e.name.c_str());
		st.eulerDeg = EulerDegreesFromQuat(e.local.rotation);
	}
	ImGui::SetNextItemWidth(200.f);
	if (ImGui::InputText("Name", st.rename, sizeof(st.rename)))
		e.name = st.rename;
	ImGui::SetNextItemWidth(240.f);
	ImGui::DragFloat3("Position in system (m)", &e.local.position.x, 0.01f);
	ImGui::SetNextItemWidth(240.f);
	if (ImGui::DragFloat3("Rotation (pitch, yaw, roll deg)", &st.eulerDeg.x, 0.5f))
		e.local.rotation = QuatFromEulerDegrees(st.eulerDeg);
	ImGui::SetNextItemWidth(240.f);
	ImGui::DragFloat3("Scale in system", &e.local.scale.x, 0.01f, 0.01f, 100.f);
}
