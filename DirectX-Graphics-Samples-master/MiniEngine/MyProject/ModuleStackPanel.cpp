#include "pch.h"
#include "ModuleStackPanel.h"

#include "EmitterRuntime.h"
#include "ModuleLibrary.h"
#include "UberGpuProgram.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cctype>

namespace
{
	const std::vector<ModuleDesc>& ModuleCatalog()
	{
		static const std::vector<ModuleDesc> s_Catalog =
		{
			SpawnRate(50.f),
			InitializeParticle(0.1f, 1.f, 1.f, float3{ 0.f, 0.f, 0.f }, float4{ 1.f, 1.f, 1.f, 1.f }),
			SphereLocation(0.5f, false),
			AddVelocityInCone(3.f, 30.f, float3{ 0.f, 1.f, 0.f }),
			ParticleState(true),
			GravityForce(float3{ 0.f, -9.8f, 0.f }),
			SolveForceAndVelocity(0.f, 0.f),      // 0 = 제한 없음
			SubUVAnimation(0.f, 63.f)
		};
		return s_Catalog;
	}

	bool HasModuleId(const std::vector<ModuleDesc>& modules, ModuleId id)
	{
		for (const ModuleDesc& m : modules)
			if (m.id == id)
				return true;
		return false;
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

	// ---- 스프라이트 텍스처 (T1) ----
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
				dds.push_back(name);
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

	void DrawTexture(EmitterRuntime& runtime)
	{
		const std::string& current = runtime.SpriteTexture();
		const std::string preview = current.empty() ? std::string("(soft circle)") : current;

		ImGui::SetNextItemWidth(300.f);
		if (ImGui::BeginCombo("Texture", preview.c_str()))
		{
			if (ImGui::Selectable("(soft circle)", current.empty()))
				runtime.SetSpriteTexture(std::string());

			// 펼칠 때마다 다시 읽는다 (파일을 추가해도 재실행 필요 없음)
			for (const std::string& path : ListTextureFiles())
				if (ImGui::Selectable(path.c_str(), path == current))
					runtime.SetSpriteTexture(path);
			ImGui::EndCombo();
		}
	}

	// ---- SubUV / 블렌드 (T2) ----
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

		if (changed)
			runtime.SetSpriteSettings(s);
	}

	// 렌더러 바인딩 (R1): 칸마다 [연결할 속성 콤보] + [기본값]
	void DrawBindings(EmitterRuntime& runtime)
	{
		const RendererDesc& desc = runtime.SpriteBindings();
		ImGui::SeparatorText(desc.name.c_str());

		DrawTexture(runtime);
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

	void DrawParam(EmitterRuntime& runtime, size_t moduleIndex, size_t paramIndex)
	{
		ModuleDesc& module = runtime.EditModules()[moduleIndex];
		ParamDesc& p = module.params[paramIndex];

		ImGui::PushID(static_cast<int>(paramIndex));
		ImGui::TextUnformatted(p.name.c_str());

		if (module.stage == Stage::ParticleSpawn && IsFloatFamily(p.type))
		{
			static const char* const kKinds[] = { "Constant", "Random Range" };
			int kind = static_cast<int>(p.kind);

			ImGui::SameLine();
			ImGui::SetNextItemWidth(130.f);
			if (ImGui::Combo("##kind", &kind, kKinds, IM_ARRAYSIZE(kKinds)))
				runtime.SetParamKind(moduleIndex, paramIndex, static_cast<ParamKind>(kind));
		}

		bool changed = false;
		if (p.kind == ParamKind::Constant)
		{
			changed = DrawValue("##value", p, p.value.data());
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
}

void DrawModuleStackPanel(EmitterRuntime& runtime)
{
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
			// 이미 있는 모듈은 추가할 수 있지만, 공유 셰이더에는 자리가 하나뿐이라 생성 방식으로 실행된다
			const bool dup = HasModuleId(runtime.EditModules(), catalog[c].id);
			const std::string label = catalog[c].name + (dup ? "  (already in stack: runs Generated)" : "");
			if (ImGui::Selectable(label.c_str()))
				runtime.AddModule(catalog[c]);
		}
		ImGui::EndCombo();
	}

	DrawBindings(runtime);
}
