#include "pch.h"
#include "EmitterAsset.h"

#include "Json.h"
#include "ModuleLibrary.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace
{
	const char* const kFormat = "MyProject.Emitter";
	const char* const kDirectory = "Assets/Emitters/";
	const char* const kExtension = ".emitter.json";
	const uint32_t    kMaxCapacity = 16u * 1024u * 1024u;   // 잘못된 파일로 거대한 버퍼를 잡지 않게

	// ---- enum <-> 이름 (파일에 쓰는 이름. 바꾸면 기존 파일을 못 읽는다) ----
	const char* const kBlendNames[] = { "Translucent", "Additive" };
	const char* const kFacingNames[] = { "FaceCameraPlane", "FaceCameraPosition", "CustomFacingVector", "FaceCameraDistanceBlend" };
	const char* const kAlignmentNames[] = { "Unaligned", "VelocityAligned", "CustomAlignment" };
	const char* const kTextureAlphaNames[] = { "Straight", "Premultiplied", "FromLuminance" };
	const char* const kColorSpaceNames[] = { "sRGB", "Linear" };

	static_assert(_countof(kFacingNames) == size_t(SpriteFacingMode::Count), "SpriteFacingMode 이름 표");
	static_assert(_countof(kAlignmentNames) == size_t(SpriteAlignment::Count), "SpriteAlignment 이름 표");
	static_assert(_countof(kTextureAlphaNames) == size_t(SpriteTextureAlpha::Count), "SpriteTextureAlpha 이름 표");
	static_assert(_countof(kColorSpaceNames) == size_t(TextureColorSpace::Count), "TextureColorSpace 이름 표");

	template<size_t N>
	bool EnumFromName(const JsonValue* v, const char* const (&names)[N], uint32_t& out, const char* what, std::vector<std::string>& warnings)
	{
		if (v == nullptr)
			return false;
		for (uint32_t i = 0; i < N; ++i)
		{
			if (v->AsString() == names[i])
			{
				out = i;
				return true;
			}
		}
		warnings.push_back(std::string("unknown ") + what + " '" + v->AsString() + "' -> kept default");
		return false;
	}

	// ---- 값 하나 (TypeInfo 크기만큼의 바이트) <-> JSON ----
	uint32_t FloatCount(const TypeInfo* type) { return type->size / uint32_t(sizeof(float)); }

	JsonValue EncodeValue(const TypeInfo* type, const uint8_t* data)
	{
		if (type == &Types::Bool || type == &Types::Int32)
		{
			int32_t v = 0;
			std::memcpy(&v, data, sizeof(v));
			return (type == &Types::Bool) ? JsonValue(v != 0) : JsonValue(int(v));
		}
		if (type == &Types::UInt32)
		{
			uint32_t v = 0;
			std::memcpy(&v, data, sizeof(v));
			return JsonValue(v);
		}
		if (IsFloatFamily(type))
		{
			float f[4] = {};
			std::memcpy(f, data, type->size);
			const uint32_t n = FloatCount(type);
			if (n == 1)
				return JsonValue(f[0]);
			JsonValue arr = JsonValue::MakeArray();
			for (uint32_t i = 0; i < n; ++i)
				arr.Push(JsonValue(f[i]));
			return arr;
		}
		return JsonValue();   // 행렬 등은 입력값으로 쓰지 않는다
	}

	// out 은 type->size 바이트. 모양이 맞지 않으면 false (out 은 건드리지 않음)
	bool DecodeValue(const JsonValue& v, const TypeInfo* type, uint8_t* out)
	{
		if (type == &Types::Bool)
		{
			if (!v.IsBool() && !v.IsNumber())
				return false;
			const int32_t b = (v.IsBool() ? v.AsBool() : v.AsNumber() != 0.0) ? 1 : 0;
			std::memcpy(out, &b, sizeof(b));
			return true;
		}
		if (type == &Types::Int32 || type == &Types::UInt32)
		{
			if (!v.IsNumber())
				return false;
			if (type == &Types::Int32)
			{
				const int32_t i = int32_t(v.AsNumber());
				std::memcpy(out, &i, sizeof(i));
			}
			else
			{
				const uint32_t u = uint32_t((std::max)(v.AsNumber(), 0.0));
				std::memcpy(out, &u, sizeof(u));
			}
			return true;
		}
		if (IsFloatFamily(type))
		{
			const uint32_t n = FloatCount(type);
			float f[4] = {};
			if (n == 1)
			{
				if (!v.IsNumber())
					return false;
				f[0] = float(v.AsNumber());
			}
			else
			{
				if (!v.IsArray() || v.Size() != n)
					return false;
				for (uint32_t i = 0; i < n; ++i)
				{
					if (!v.At(i).IsNumber())
						return false;
					f[i] = float(v.At(i).AsNumber());
				}
			}
			std::memcpy(out, f, type->size);
			return true;
		}
		return false;
	}

	// ---- 모듈 입력값 ----
	//   Constant    : 값 그대로             "Rate": 50
	//   RandomRange : { min, max, perComponent }
	//   Curve       : { value, curve: [ { time, value }, ... ], index }   (value = 상수로 되돌릴 때의 값)
	JsonValue EncodeParam(const ParamDesc& p)
	{
		if (p.kind == ParamKind::Curve && p.curve)
		{
			JsonValue obj = JsonValue::MakeObject();
			obj.Set("value", EncodeValue(p.type, p.value.data()));
			JsonValue keys = JsonValue::MakeArray();
			for (const CurveKey& k : p.curve->Keys())   // 저장된 순서 그대로 (구울 때 정렬한다)
			{
				JsonValue key = JsonValue::MakeObject();
				key.Set("time", JsonValue(k.time));
				key.Set("value", EncodeValue(p.type, reinterpret_cast<const uint8_t*>(&k.value)));
				keys.Push(std::move(key));
			}
			obj.Set("curve", std::move(keys));
			obj.Set("index", JsonValue(p.curveIndex));
			return obj;
		}
		if (p.kind != ParamKind::RandomRange)
			return EncodeValue(p.type, p.value.data());

		JsonValue obj = JsonValue::MakeObject();
		obj.Set("min", EncodeValue(p.type, p.value.data()));
		obj.Set("max", EncodeValue(p.type, p.value.data() + p.type->size));
		if (p.type->size > sizeof(float))
			obj.Set("perComponent", JsonValue(p.perComponent));
		return obj;
	}

	void ClampToMin(ParamDesc& p, uint8_t* data, const std::string& where, std::vector<std::string>& warnings)
	{
		if (!p.uiHasMin || !IsFloatFamily(p.type))
			return;
		float f[4] = {};
		std::memcpy(f, data, p.type->size);
		bool clamped = false;
		for (uint32_t i = 0; i < FloatCount(p.type); ++i)
		{
			if (f[i] < p.uiMin)
			{
				f[i] = p.uiMin;
				clamped = true;
			}
		}
		if (clamped)
		{
			std::memcpy(data, f, p.type->size);
			warnings.push_back(where + ": below minimum -> clamped");
		}
	}

	void DecodeCurveParam(const JsonValue& v, ParamDesc& p, bool curveAllowed, const std::string& where, std::vector<std::string>& warnings)
	{
		const size_t size = p.type->size;
		const JsonValue& keysV = *v.Find("curve");

		std::vector<CurveKey> keys;
		if (keysV.IsArray())
		{
			for (size_t i = 0; i < keysV.Size(); ++i)
			{
				const JsonValue& kv = keysV.At(i);
				const JsonValue* t = kv.IsObject() ? kv.Find("time") : nullptr;
				const JsonValue* val = kv.IsObject() ? kv.Find("value") : nullptr;
				CurveKey key;
				key.value = float4{ 0.f, 0.f, 0.f, 0.f };
				if (t == nullptr || !t->IsNumber() || val == nullptr || !DecodeValue(*val, p.type, reinterpret_cast<uint8_t*>(&key.value)))
				{
					warnings.push_back(where + ".curve[" + std::to_string(i) + "]: wrong shape -> skipped");
					continue;
				}
				key.time = float(t->AsNumber());
				keys.push_back(key);
			}
		}
		if (keys.empty())
		{
			warnings.push_back(where + ": curve has no valid keys -> kept default");
			return;
		}

		// 상수로 되돌릴 때 쓸 값. 없으면 첫 키 값
		std::vector<uint8_t> value(size);
		const JsonValue* constV = v.Find("value");
		if (constV == nullptr || !DecodeValue(*constV, p.type, value.data()))
			std::memcpy(value.data(), &keys.front().value, size);

		if (!curveAllowed)
		{
			warnings.push_back(where + ": Curve is not allowed here -> used value as a constant");
			p.kind = ParamKind::Constant;
			p.perComponent = false;
			p.curve.reset();
			p.value = std::move(value);
			return;
		}

		const JsonValue* indexV = v.Find("index");
		if (indexV != nullptr && indexV->IsString() && !indexV->AsString().empty())
			p.curveIndex = indexV->AsString();

		p.kind = ParamKind::Curve;
		p.perComponent = false;
		p.value = std::move(value);
		p.curve = std::make_shared<CurveAsset>(keys);   // 새 칸 (기본값 모듈의 커브와 따로)
	}

	void DecodeParam(const JsonValue& v, const ModuleDesc& module, ParamDesc& p, std::vector<std::string>& warnings)
	{
		const std::string where = std::string(ModuleIdName(module.id)) + "." + p.name;
		const size_t size = p.type->size;
		const bool wantRange = v.IsObject() && v.Find("min") != nullptr;
		// UI 와 같은 규칙: RandomRange 는 ParticleSpawn 의 float 계열만
		const bool rangeAllowed = module.stage == Stage::ParticleSpawn && IsFloatFamily(p.type);
		// Curve 는 ParticleUpdate 의 float 계열만 (EmitterCompiler / UI 와 같은 규칙)
		const bool wantCurve = v.IsObject() && v.Find("curve") != nullptr;
		const bool curveAllowed = module.stage == Stage::ParticleUpdate && IsFloatFamily(p.type);

		std::vector<uint8_t> value(size);
		if (wantCurve)
		{
			DecodeCurveParam(v, p, curveAllowed, where, warnings);
			return;
		}
		p.curve.reset();    // 기본값이 Curve 였던 입력값(ScaleColor.ScaleAlpha 등)을 상수 / 범위로 덮을 때 칸을 돌려준다
		if (!wantRange)
		{
			if (!DecodeValue(v, p.type, value.data()))
			{
				warnings.push_back(where + ": value has the wrong shape -> kept default");
				return;
			}
			ClampToMin(p, value.data(), where, warnings);
			p.kind = ParamKind::Constant;
			p.perComponent = false;
			p.value = std::move(value);
			return;
		}

		const JsonValue* minV = v.Find("min");
		const JsonValue* maxV = v.Find("max");
		if (!DecodeValue(*minV, p.type, value.data()))
		{
			warnings.push_back(where + ": min has the wrong shape -> kept default");
			return;
		}
		ClampToMin(p, value.data(), where + ".min", warnings);

		if (!rangeAllowed)
		{
			warnings.push_back(where + ": Random Range is not allowed here -> used min as a constant");
			p.kind = ParamKind::Constant;
			p.perComponent = false;
			p.value = std::move(value);
			return;
		}

		value.resize(size * 2);
		if (maxV == nullptr || !DecodeValue(*maxV, p.type, value.data() + size))
		{
			warnings.push_back(where + ": max is missing or has the wrong shape -> max = min");
			std::memcpy(value.data() + size, value.data(), size);
		}
		ClampToMin(p, value.data() + size, where + ".max", warnings);

		const JsonValue* perComp = v.Find("perComponent");
		p.kind = ParamKind::RandomRange;
		p.perComponent = (size > sizeof(float)) && perComp != nullptr && perComp->AsBool();
		p.value = std::move(value);
	}

	JsonValue Float2Json(float2 v)
	{
		JsonValue a = JsonValue::MakeArray();
		a.Push(JsonValue(v.x));
		a.Push(JsonValue(v.y));
		return a;
	}

	void ReadFloat(const JsonValue& obj, const char* key, float& out, std::vector<std::string>& warnings)
	{
		const JsonValue* v = obj.Find(key);
		if (v == nullptr)
			return;
		if (v->IsNumber())
			out = float(v->AsNumber());
		else
			warnings.push_back(std::string("sprite.") + key + ": not a number -> kept default");
	}
}

EmitterAsset::EmitterAsset()
{
	for (uint32_t i = 0; i < ParticleMaterial::kNumTextureSlot; ++i)
		textures[i] = ParticleMaterial::DefaultTexture();
	for (uint32_t i = 0; i < ParticleMaterial::kMaxParams; ++i)
		materialParams[i] = ParticleMaterial::DefaultParam(i);
}

namespace
{
	bool ApplyTextures(EmitterRuntime& runtime, const EmitterAsset& asset, std::vector<std::string>* failed)
	{
		bool ok = true;
		for (uint32_t i = 0; i < ParticleMaterial::kNumTextureSlot; ++i)
		{
			if (runtime.SetMaterialTexture(i, asset.textures[i]))
				continue;
			ok = false;
			if (failed)
				failed->push_back(asset.textures[i].path);
		}
		return ok;
	}
	
	void ApplyMaterialParams(EmitterRuntime& runtime, const EmitterAsset& asset)
	{
		for (uint32_t k = 0; k < ParticleMaterial::kMaxParams; ++k)
			runtime.SetMaterialParam(k, asset.materialParams[k]);
	}

	void ApplyMaterialLayers(EmitterRuntime& runtime, const EmitterAsset& asset)
	{
		if (!runtime.SetMaterialLayers(asset.materialLayers))
		{
			Utility::Printf("[EmitterAsset] '%s': material layers rejected (%s) -> no effect\n", asset.name.c_str(),
				runtime.Material().LayerError().c_str());

		if (asset.hasMaterialGraph)
		{
			runtime.SetMaterialGraph(asset.materialGraph);
			if (!runtime.Material().ProgramError().empty())
				Utility::Printf("[EmitterAsset] '%s': material graph does not compile (%s) -> base color\n", asset.name.c_str(),
					runtime.Material().ProgramError().c_str());
		}
		else
			runtime.ClearMaterialGraph();
		}
	}
}

// ------------------------------------------------------------------ 캡처 / 적용

EmitterAsset CaptureEmitterAsset(const EmitterRuntime& runtime, const std::string& name)
{
	EmitterAsset a;
	a.name = name;
	a.capacity = runtime.Capacity();
	a.backend = runtime.Backend();
	a.modules = runtime.Modules();
	a.sprite = runtime.SpriteBindings();

	for (uint32_t i = 0; i < ParticleMaterial::kNumTextureSlot; ++i)
		a.textures[i] = runtime.Material().SlotTexture(i);
	for (uint32_t i = 0; i < ParticleMaterial::kMaxParams; ++i)
		a.materialParams[i] = runtime.Material().Param(i);
	a.materialLayers = runtime.Material().Layers();
	a.hasMaterialGraph = runtime.Material().HasCustomGraph();
	if (a.hasMaterialGraph)
		a.materialGraph = *runtime.Material().CustomGraph();
	a.settings = runtime.GetSpriteSettings();
	return a;
}

bool ApplyEmitterAsset(EmitterRuntime& runtime, const EmitterAsset& asset, std::vector<std::string>* failedTextures)
{
	runtime.ReplaceDescription(asset.modules, asset.sprite, asset.capacity, asset.backend);
	runtime.SetSpriteSettings(asset.settings);
	ApplyMaterialLayers(runtime, asset);
	ApplyMaterialParams(runtime, asset);
	return ApplyTextures(runtime, asset, failedTextures);
}

bool InitializeEmitterFromAsset(EmitterRuntime& runtime, const EmitterAsset& asset, uint32_t capacity)
{
	if (capacity == 0)
		capacity = asset.capacity;
	if (capacity == 0)
		capacity = 4096;   // 파일에도 없으면 테스트 씬 기본값

	if (!runtime.Initialize(asset.modules, asset.sprite, capacity, asset.backend))
		return false;

	// 둘 다 재시작이 필요 없는 값이라 시작한 뒤에 넣는다
	runtime.SetSpriteSettings(asset.settings);
	ApplyMaterialLayers(runtime, asset);
	ApplyMaterialParams(runtime, asset);

	std::vector<std::string> failed;
	if (!ApplyTextures(runtime, asset, &failed))
	{
		for (const std::string& path : failed)
			Utility::Printf("[EmitterAsset] '%s': texture '%s' could not be loaded (drawn magenta)\n", asset.name.c_str(), path.c_str());
	}	return true;
}

// ------------------------------------------------------------------ JSON

std::string EmitterAssetToJson(const EmitterAsset& a)
{
	JsonValue root = JsonValue::MakeObject();
	root.Set("format", JsonValue(kFormat));
	root.Set("version", JsonValue(EmitterAsset::kVersion));
	root.Set("name", JsonValue(a.name));
	root.Set("capacity", JsonValue(a.capacity));
	root.Set("backend", JsonValue(a.backend == EmitterBackend::Cpu ? "Cpu" : "Gpu"));

	JsonValue modules = JsonValue::MakeArray();
	for (const ModuleDesc& m : a.modules)
	{
		if (m.id == ModuleId::Unknown)
			continue;   // ModuleLibrary 밖에서 만든 모듈은 다시 만들 수 없다

		JsonValue jm = JsonValue::MakeObject();
		jm.Set("module", JsonValue(ModuleIdName(m.id)));
		JsonValue params = JsonValue::MakeObject();
		for (const ParamDesc& p : m.params)
			params.Set(p.name, EncodeParam(p));
		jm.Set("params", std::move(params));
		modules.Push(std::move(jm));
	}
	root.Set("modules", std::move(modules));

	const SpriteSettings& s = a.settings;
	JsonValue sprite = JsonValue::MakeObject();
	JsonValue textures = JsonValue::MakeArray();
	for (uint32_t i = 0; i < ParticleMaterial::kNumTextureSlot; ++i)
	{
		JsonValue jt = JsonValue::MakeObject();
		jt.Set("slot", JsonValue(ParticleMaterial::SlotName(i)));     // 읽을 때는 쓰지 않음 (사람이 보기 위한 것)
		jt.Set("path", JsonValue(a.textures[i].path));
		jt.Set("colorSpace", JsonValue(kColorSpaceNames[(std::min)(uint32_t(a.textures[i].colorSpace), uint32_t(TextureColorSpace::Count) - 1)]));
		textures.Push(std::move(jt));
	}
	sprite.Set("textures", std::move(textures));
	JsonValue params = JsonValue::MakeArray();
	for (uint32_t k = 0; k < ParticleMaterial::kMaxParams; ++k)
		params.Push(EncodeValue(&Types::Float4, reinterpret_cast<const uint8_t*>(&a.materialParams[k])));
	sprite.Set("materialParams", std::move(params));
	
	JsonValue layers = JsonValue::MakeArray();
	for (const ParticleMaterial::Effect effect : a.materialLayers)
		layers.Push(JsonValue(ParticleMaterial::EffectName(effect)));
	sprite.Set("materialLayers", std::move(layers));
	if (a.hasMaterialGraph)
		sprite.Set("materialGraph", MaterialGraphToJson(a.materialGraph));

	sprite.Set("textureAlpha", JsonValue(kTextureAlphaNames[(std::min)(uint32_t(s.textureAlpha), uint32_t(SpriteTextureAlpha::Count) - 1)]));
	sprite.Set("blend", JsonValue(kBlendNames[(std::min)(uint32_t(s.blend), 1u)]));
	JsonValue grid = JsonValue::MakeArray();
	grid.Push(JsonValue(s.subImageCols));
	grid.Push(JsonValue(s.subImageRows));
	sprite.Set("subImage", std::move(grid));
	sprite.Set("subUVBlend", JsonValue(s.subUVBlend));
	sprite.Set("facing", JsonValue(kFacingNames[(std::min)(uint32_t(s.facing), uint32_t(SpriteFacingMode::Count) - 1)]));
	sprite.Set("alignment", JsonValue(kAlignmentNames[(std::min)(uint32_t(s.alignment), uint32_t(SpriteAlignment::Count) - 1)]));
	sprite.Set("pivot", Float2Json(s.pivot));
	sprite.Set("cameraOffset", JsonValue(s.cameraOffset));
	sprite.Set("velocityStretch", JsonValue(s.velocityStretch));
	sprite.Set("blendNear", JsonValue(s.blendNear));
	sprite.Set("blendFar", JsonValue(s.blendFar));

	JsonValue bindings = JsonValue::MakeObject();
	for (const RenderBinding& b : a.sprite.bindings)
	{
		JsonValue jb = JsonValue::MakeObject();
		jb.Set("attribute", JsonValue(b.attribute));   // 빈 문자열 = 항상 기본값
		jb.Set("default", EncodeValue(b.type, b.defaultValue.data()));
		bindings.Set(b.slot, std::move(jb));
	}
	sprite.Set("bindings", std::move(bindings));
	root.Set("sprite", std::move(sprite));

	return WriteJson(root);
}

bool EmitterAssetFromJson(const std::string& text, EmitterAsset& out, std::string& error, std::vector<std::string>& warnings)
{
	JsonValue root;
	if (!ParseJson(text, root, error))
		return false;
	if (!root.IsObject())
	{
		error = "root is not an object";
		return false;
	}

	const JsonValue* format = root.Find("format");
	if (format == nullptr || format->AsString() != kFormat)
	{
		error = std::string("not an emitter asset (format must be \"") + kFormat + "\")";
		return false;
	}
	const JsonValue* version = root.Find("version");
	if (version == nullptr || !version->IsNumber())
		warnings.push_back("version missing -> read as version 1");
	else if (version->AsNumber() > double(EmitterAsset::kVersion))
	{
		error = "file version " + std::to_string(int(version->AsNumber())) + " is newer than this build ("
			+ std::to_string(EmitterAsset::kVersion) + ")";
		return false;
	}

	EmitterAsset a;   // 끝까지 성공해야 out 에 넘긴다

	if (const JsonValue* v = root.Find("name"))
		a.name = v->AsString();

	if (const JsonValue* v = root.Find("capacity"))
	{
		const double c = v->AsNumber(0.0);
		if (c < 0.0 || c > double(kMaxCapacity))
			warnings.push_back("capacity out of range -> kept current");
		else
			a.capacity = uint32_t(c);
	}

	if (const JsonValue* v = root.Find("backend"))
	{
		if (v->AsString() == "Cpu")
			a.backend = EmitterBackend::Cpu;
		else if (v->AsString() != "Gpu")
			warnings.push_back("unknown backend '" + v->AsString() + "' -> Gpu");
	}

	// ---- 모듈 ----
	const JsonValue* modules = root.Find("modules");
	if (modules == nullptr || !modules->IsArray())
	{
		error = "\"modules\" array is missing";
		return false;
	}
	for (size_t i = 0; i < modules->Size(); ++i)
	{
		const JsonValue& jm = modules->At(i);
		const JsonValue* idV = jm.Find("module");
		const std::string idName = idV ? idV->AsString() : std::string();
		const ModuleId id = ModuleIdFromName(idName);
		if (id == ModuleId::Unknown)
		{
			// 모듈 하나가 빠지면 동작이 달라지므로 일부만 불러오지 않는다
			error = "modules[" + std::to_string(i) + "]: unknown module '" + idName + "'";
			return false;
		}

		ModuleDesc m = MakeDefaultModule(id);
		if (const JsonValue* params = jm.Find("params"))
		{
			for (const JsonValue::Member& member : params->Members())
			{
				auto it = std::find_if(m.params.begin(), m.params.end(),
					[&](const ParamDesc& p) { return p.name == member.first; });
				if (it == m.params.end())
				{
					warnings.push_back(idName + ": unknown param '" + member.first + "' -> ignored");
					continue;
				}
				DecodeParam(member.second, m, *it, warnings);
			}
		}
		a.modules.push_back(std::move(m));
	}

	// ---- 스프라이트 ----
	if (const JsonValue* sprite = root.Find("sprite"))
	{
		SpriteSettings& s = a.settings;

		uint32_t e = 0;

		if (const JsonValue* textures = sprite->Find("textures"))
		{
			if (textures->IsArray())
			{
				if (textures->Size() > ParticleMaterial::kNumTextureSlot)
					warnings.push_back("sprite.textures: more than " + std::to_string(ParticleMaterial::kNumTextureSlot) + " slots -> extra ignored");
				const uint32_t n = min(uint32_t(textures->Size()), uint32_t(ParticleMaterial::kNumTextureSlot));
				for (uint32_t i = 0; i < n; ++i)
				{
					const JsonValue& jt = textures->At(i);
					MaterialTexture& t = a.textures[i];
					if (const JsonValue* v = jt.Find("path"))
						t.path = v->AsString();
					if (EnumFromName(jt.Find("colorSpace"), kColorSpaceNames, e, "colorSpace", warnings))
						t.colorSpace = TextureColorSpace(e);
				}
			}
			else
				warnings.push_back("sprite.textures: expected an array -> kept default");
		}
		else if (const JsonValue* v = sprite->Find("texture"))
		{
			a.textures[ParticleMaterial::kBaseTexture].path = v->AsString();
			a.textures[ParticleMaterial::kBaseTexture].colorSpace = TextureColorSpace::sRGB;
		}

		if (const JsonValue* params = sprite->Find("materialParams"))
		{
			if (params->IsArray())
			{
				if (params->Size() > ParticleMaterial::kMaxParams)
					warnings.push_back("sprite.materialParams: more than " + std::to_string(ParticleMaterial::kMaxParams) + " -> extra ignored");
				const uint32_t n = (std::min)(uint32_t(params->Size()), uint32_t(ParticleMaterial::kMaxParams));
				for (uint32_t k = 0; k < n; ++k)
				{
					if (!DecodeValue(params->At(k), &Types::Float4, reinterpret_cast<uint8_t*>(&a.materialParams[k])))
						warnings.push_back("sprite.materialParams[" + std::to_string(k) + "]: expected [x, y, z, w] -> kept default");
				}
			}
			else
				warnings.push_back("sprite.materialParams: expected an array -> kept default");
		}

		if (const JsonValue* layers = sprite->Find("materialLayers"))
		{
			if (layers->IsArray())
			{
				std::vector<ParticleMaterial::Effect> list;
				for (size_t i = 0; i < layers->Size(); ++i)
				{
					const std::string& name = layers->At(i).AsString();
					bool found = false;
					for (uint32_t k = 1; k < uint32_t(ParticleMaterial::Effect::Count); ++k)
					{
						if (name == ParticleMaterial::EffectName(ParticleMaterial::Effect(k)))
						{
							list.push_back(ParticleMaterial::Effect(k));
							found = true;
							break;
						}
					}
					if (!found)
						warnings.push_back("sprite.materialLayers[" + std::to_string(i) + "]: unknown effect '" + name + "' -> layer skipped");
				}
				std::string layerError;
				if (ParticleMaterial::LayoutLayers(list, nullptr, &layerError))
					a.materialLayers = list;
				else
					warnings.push_back("sprite.materialLayers: " + layerError + " -> no layers");
			}
			else
				warnings.push_back("sprite.materialLayers: expected an array -> no layers");
		}

		if (const JsonValue* graph = sprite->Find("materialGraph"))
		{
			MaterialGraph g;
			if (MaterialGraphFromJson(*graph, g, warnings))
			{
				a.hasMaterialGraph = true;
				a.materialGraph = std::move(g);
			}
		}

		if (EnumFromName(sprite->Find("textureAlpha"), kTextureAlphaNames, e, "textureAlpha", warnings))
			s.textureAlpha = SpriteTextureAlpha(e);
		if (EnumFromName(sprite->Find("blend"), kBlendNames, e, "blend", warnings))
			s.blend = SpriteBlendMode(e);
		if (EnumFromName(sprite->Find("facing"), kFacingNames, e, "facing", warnings))
			s.facing = SpriteFacingMode(e);
		if (EnumFromName(sprite->Find("alignment"), kAlignmentNames, e, "alignment", warnings))
			s.alignment = SpriteAlignment(e);

		if (const JsonValue* v = sprite->Find("subImage"))
		{
			if (v->IsArray() && v->Size() == 2 && v->At(0).IsNumber() && v->At(1).IsNumber())
			{
				// UI 범위와 같게 1 ~ 64
				s.subImageCols = uint32_t((std::min)((std::max)(v->At(0).AsNumber(), 1.0), 64.0));
				s.subImageRows = uint32_t((std::min)((std::max)(v->At(1).AsNumber(), 1.0), 64.0));
			}
			else
				warnings.push_back("sprite.subImage: expected [cols, rows] -> kept default");
		}
		if (const JsonValue* v = sprite->Find("subUVBlend"))
			s.subUVBlend = v->AsBool(s.subUVBlend);

		if (const JsonValue* v = sprite->Find("pivot"))
		{
			float p[2] = {};
			if (DecodeValue(*v, &Types::Float2, reinterpret_cast<uint8_t*>(p)))
				s.pivot = { p[0], p[1] };
			else
				warnings.push_back("sprite.pivot: expected [x, y] -> kept default");
		}
		ReadFloat(*sprite, "cameraOffset", s.cameraOffset, warnings);
		ReadFloat(*sprite, "velocityStretch", s.velocityStretch, warnings);
		ReadFloat(*sprite, "blendNear", s.blendNear, warnings);
		ReadFloat(*sprite, "blendFar", s.blendFar, warnings);
		s.blendNear = (std::max)(s.blendNear, 0.f);
		s.blendFar = (std::max)(s.blendFar, s.blendNear);

		if (const JsonValue* bindings = sprite->Find("bindings"))
		{
			for (const JsonValue::Member& member : bindings->Members())
			{
				auto it = std::find_if(a.sprite.bindings.begin(), a.sprite.bindings.end(),
					[&](const RenderBinding& b) { return b.slot == member.first; });
				if (it == a.sprite.bindings.end())
				{
					warnings.push_back("unknown binding slot '" + member.first + "' -> ignored");
					continue;
				}
				if (const JsonValue* attr = member.second.Find("attribute"))
					it->attribute = attr->AsString();   // 쓰는 모듈이 없으면 런타임이 기본값으로 그린다
				if (const JsonValue* def = member.second.Find("default"))
				{
					std::vector<uint8_t> value(it->type->size);
					if (DecodeValue(*def, it->type, value.data()))
						it->defaultValue = std::move(value);
					else
						warnings.push_back("binding '" + member.first + "': default has the wrong shape -> kept default");
				}
			}
		}
	}

	out = std::move(a);
	return true;
}

// ------------------------------------------------------------------ 파일

bool SaveEmitterAsset(const std::string& path, const EmitterAsset& asset, std::string& error)
{
	const size_t slash = path.find_last_of("/\\");
	if (slash != std::string::npos)
		CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);   // 이미 있으면 실패해도 괜찮다

	const std::string temp = path + ".tmp";
	{
		std::ofstream file(temp, std::ios::out | std::ios::trunc);   // 텍스트 모드 = CRLF
		if (!file)
		{
			error = "cannot open " + temp + " for writing";
			return false;
		}
		file << EmitterAssetToJson(asset);
		file.flush();
		if (!file)
		{
			error = "write failed: " + temp;
			return false;
		}
	}

	if (!MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
	{
		DeleteFileA(temp.c_str());
		error = "cannot replace " + path + " (open in another program?)";
		return false;
	}
	return true;
}

bool LoadEmitterAsset(const std::string& path, EmitterAsset& out, std::string& error, std::vector<std::string>& warnings)
{
	std::ifstream file(path, std::ios::in | std::ios::binary);
	if (!file)
	{
		error = "cannot open " + path;
		return false;
	}
	std::ostringstream ss;
	ss << file.rdbuf();
	std::string text = ss.str();
	if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
		text.erase(0, 3);   // 메모장 등이 붙인 BOM

	return EmitterAssetFromJson(text, out, error, warnings);
}

const char* EmitterAssetDirectory() { return kDirectory; }
const char* EmitterAssetExtension() { return kExtension; }

std::string EmitterAssetPath(const std::string& name)
{
	return std::string(kDirectory) + name + kExtension;
}

bool IsValidEmitterAssetName(const std::string& name)
{
	if (name.empty() || name.size() > 64 || name[0] == '.')
		return false;
	for (const char c : name)
	{
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
			|| c == '_' || c == '-' || c == '.';
		if (!ok)
			return false;
	}
	return true;
}

std::vector<std::string> ListEmitterAssets()
{
	std::vector<std::string> names;
	const std::string ext = kExtension;

	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((std::string(kDirectory) + "*" + ext).c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return names;
	do
	{
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		const std::string file = fd.cFileName;
		if (file.size() > ext.size() && file.compare(file.size() - ext.size(), ext.size(), ext) == 0)
			names.push_back(file.substr(0, file.size() - ext.size()));
	} while (FindNextFileA(h, &fd));
	FindClose(h);

	std::sort(names.begin(), names.end());
	return names;
}