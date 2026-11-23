#include "pch.h"
#include "ModuleStackPanel.h"

#include "EmitterAsset.h"
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
			if (ImGui::Button("Back to layers"))
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
		const uint32_t change = MaterialGraphEditor::Draw(editable ? "edit" : "view", graph, compiled, editable);
		if (editable && (change & MaterialGraphEditor::kGraph))
			runtime.SetMaterialGraph(graph);
		else if (editable && (change & MaterialGraphEditor::kLayout))
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

			ImGui::Checkbox("Graph Editor", &s_ShowGraphEditor);
			ImGui::SameLine();
			if (material.HasCustomGraph())
			{
				if (ImGui::Button("Back to layers"))
					runtime.ClearMaterialGraph();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Discard the custom graph and draw with the effect layers again.\nParam slots reset to the layer defaults.");
				ImGui::SameLine();
				ImGui::TextDisabled("(custom graph)");
			}
			else if (ImGui::Button("Edit as graph"))
			{
				runtime.SetMaterialGraph(material.LayersAsGraph());
				s_ShowGraphEditor = true;
			}

			if (!material.HasCustomGraph())
			{
			ImGui::Text("Effect Layers (%u / %u)", uint32_t(material.Layers().size()), ParticleMaterial::kMaxLayers);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Applied top to bottom: each layer's result is the next layer's base color.\n"
					"Coverage (shape alpha) is always the original sprite shape.\n"
					"Pattern: color x Tex.r / Tint: color x Tex.rgb / Dissolve: Tex = mask, progress = ParticleParam0\n"
					"Distortion: first layer only (re-reads the base texture) / Glow: Tex x Tex -> add color\n"
					"Limits: 4 layers, 4 effect textures, 16 param slots");

			for (uint32_t i = 0; i < uint32_t(material.LayerLayouts().size()); ++i)
			{
				const ParticleMaterial::LayerLayout l = material.LayerLayouts()[i];	// 복사: 아래에서 층이 바뀌면 목록이 바뀐다
				bool edited = false;
				ImGui::PushID(static_cast<int>(i));
				int effect = static_cast<int>(l.effect) - 1;
				ImGui::SetNextItemWidth(120.f);
				if (ImGui::Combo("##effect", &effect, names, static_cast<int>(numEffects)))
				{
					runtime.SetMaterialLayerEffect(i, Effect(effect + 1));		// 실패해도(한도) 이유는 LayerError 로 보인다
					edited = true;
				}
				ImGui::SameLine();
				if (ImGui::ArrowButton("##up", ImGuiDir_Up))
				{
					runtime.MoveMaterialLayer(i, -1);		// 실패해도(한도) 이유는 LayerError 로 보인다
					edited = true;
				}
				ImGui::SameLine();
				if (ImGui::ArrowButton("##down", ImGuiDir_Down))
				{
					runtime.MoveMaterialLayer(i, +1);		// 실패해도(한도) 이유는 LayerError 로 보인다
					edited = true;
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove"))
				{
					runtime.RemoveMaterialLayer(i);		// 실패해도(한도) 이유는 LayerError 로 보인다
					edited = true;
				}
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
				
				ImGui::PopID();
				if (edited)
					break;		// 목록이 바뀌었으니 이번 프레임은 여기까지
			}

			{
				static int s_AddEffect = 0;		// 추가할 효과 (UI 상태만)
				ImGui::SetNextItemWidth(120.f);
				ImGui::Combo("##addEffect", &s_AddEffect, names, static_cast<int>(numEffects));
				ImGui::SameLine();
				if (ImGui::Button("Add Layer"))
					runtime.AddMaterialLayer(Effect(s_AddEffect + 1));
			}
			if (!material.LayerError().empty())
				ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "%s", material.LayerError().c_str());
			}

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
			ImGui::TextDisabled("Param values are edited on the Constant / UVTransform nodes (Material Graph window).");
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

	// 렌더러 바인딩 (R1): 칸마다 [연결할 속성 콤보] + [기본값]
	void DrawBindings(EmitterRuntime& runtime)
	{
		const RendererDesc& desc = runtime.SpriteBindings();
		ImGui::SeparatorText(desc.name.c_str());

		DrawTexture(runtime);
		DrawMaterialParams(runtime);
		DrawSpriteSettings(runtime);

		for (size_t i = 0; i < desc.bindings.size(); ++i)
		{
			const RenderBinding& b = desc.bindings[i];
			ImGui::PushID(static_cast<int>(i));

			// ---- 연결할 속성 ----
			const std::vector<std::string> options = runtime.BindableAttributes(b.type);
			const std::string preview = b.attribute.empty() ? std::string("(default)") : b.attribute;

			ImGui::SetNextItemWidth(220.f);
			if (ImGui::BeginCombo(b.slot.c_str(), preview.c_str()))
			{
				if (ImGui::Selectable("(default)", b.attribute.empty()))
					runtime.SetBindingAttribute(i, std::string());
				for (const std::string& name : options)
					if (ImGui::Selectable(name.c_str(), name == b.attribute))
						runtime.SetBindingAttribute(i, name);

				// 지금 연결된 속성을 쓰는 모듈이 없으면 목록에 없을 수 있다 -> 그래도 보여 준다 (기본값으로 그려짐)
				if (!b.attribute.empty() && std::find(options.begin(), options.end(), b.attribute) == options.end())
					ImGui::TextDisabled("%s (no module writes it -> default)", b.attribute.c_str());
				ImGui::EndCombo();
			}

			// ---- 기본값 (연결이 없거나, 연결된 속성을 아무도 쓰지 않을 때 사용) ----
			ParamDesc view;                      // DrawValue 를 재사용하려고 모양만 맞춘다
			view.name = b.slot;
			view.type = b.type;
			view.uiColor = (b.type == &Types::Float4);
			std::vector<uint8_t> value = b.defaultValue;

			ImGui::SameLine();
			ImGui::SetNextItemWidth(160.f);
			if (DrawValue("##default", view, value.data()))
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
}
