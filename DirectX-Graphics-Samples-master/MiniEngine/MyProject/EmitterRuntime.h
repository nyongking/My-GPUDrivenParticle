#pragma once
#include "ModuleDesc.h"
#include "CompiledEmitter.h"
#include "EmitterInstance.h"
#include "EmitterGpuProgram.h"
#include "MaterialGpuProgram.h"
#include "GpuEmitterInstance.h"
#include "ShaderCompiler.h"
#include "SpriteRenderer.h"
#include "UberEmitterData.h"
#include "ParticleMaterial.h"

#include <memory>
#include <string>
#include <vector>

class GraphicsContext;
class ComputeContext;
namespace Math { class Camera; }

enum class EmitterBackend { Cpu, Gpu };

// 모듈 스택(원본 데이터)과 실행 중인 백엔드 하나를 묶는다.
// 편집 함수는 요청만 남기고, 실제 적용은 프레임 시작의 ApplyPending() 에서 한다.
class EmitterRuntime
{
public:
	bool Initialize(const std::vector<ModuleDesc>& modules, uint32_t capacity, EmitterBackend backend);
	// 렌더러 바인딩까지 정해서 시작 (에셋에서 만들 때). 바인딩 모양이 다르면 지금 것을 유지
	bool Initialize(const std::vector<ModuleDesc>& modules, const RendererDesc& sprite, uint32_t capacity, EmitterBackend backend);
	void Shutdown();

	void Tick(ComputeContext& ctx, float deltaTime, const OwnerTransform& owner);
	void Tick(ComputeContext& ctx, float deltaTime, const float3& ownerPosition);
	void Render(GraphicsContext& gfx, const Math::Camera& camera);
	// 공용 렌더 목록에 넣기 (그리기는 list.Render). 여러 이미터를 섞어서 정렬할 때 Render 대신 쓴다
	void SubmitTo(GraphicsContext& gfx, ParticleRenderList& list);

	GpuEmitterInstance* StagedGpu() { return (m_Ready && m_ActiveBackend == EmitterBackend::Gpu) ? &m_Gpu : nullptr; }
	
	void OnSubmitted(uint64_t fence);
	void ApplyPending();

	// ---------- 편집 ----------
	std::vector<ModuleDesc>& EditModules() { return m_Modules; }
	const std::vector<ModuleDesc>& Modules() const { return m_Modules; }

	// 모듈 스택 + 렌더러 바인딩 + capacity + 백엔드를 통째로 바꾼다 (에셋 불러오기). 재시작
	//   capacity == 0 이면 지금 값 유지. 적용에 실패하면 다른 편집과 같이 이전 구성으로 돌아간다
	void ReplaceDescription(const std::vector<ModuleDesc>& modules, const RendererDesc& sprite, uint32_t capacity, EmitterBackend backend);

	void NotifyValuesChanged();     // ParamDesc::value 만 바뀜 (재컴파일 없음). 커브 키 편집은 CurveAsset::Bake 만으로 반영된다
	void SetParamKind(size_t moduleIndex, size_t paramIndex, ParamKind kind);       // 파티클 유지
	void SetPerComponent(size_t moduleIndex, size_t paramIndex, bool perComponent); // 파티클 유지

	// -------- 렌더러 기본값 -------
	// 렌더러가 읽는 속성은 고정이다 (렌더 구조체, RenderParticle.h). 여기서 바꿀 수 있는 것은
	// "아무 모듈도 그 속성을 쓰지 않을 때의 값" 뿐이다
	const RendererDesc& SpriteBindings() const { return m_SpriteDesc; }
	// 기본값 변경. 재컴파일 없음. 렌더 구조체의 값은 다음에 태어나는 파티클부터, Facing / Alignment 벡터는 바로 반영
	void SetBindingDefault(size_t bindingIndex, const std::vector<uint8_t>& value);

	// -------- Material 설정 -------
	bool SetMaterialTexture(uint32_t slot, const MaterialTexture& texture);
	const ParticleMaterial& Material() const { return m_Material; }
	void SetMaterialParam(uint32_t k, const float4& value);
	void SetMaterialEffect(ParticleMaterial::Effect effect);
	bool AddMaterialLayer(ParticleMaterial::Effect effect);
	bool RemoveMaterialLayer(uint32_t index);
	bool MoveMaterialLayer(uint32_t index, int direction);
	bool SetMaterialLayers(const std::vector<ParticleMaterial::Effect>& layers);
	bool SetMaterialLayerEffect(uint32_t index, ParticleMaterial::Effect effect);
	// 효과 층이 둘 이상이면 그래프(Custom) 로 넘긴다. 에셋을 불러온 뒤 (값까지 넣은 뒤) 부른다
	void ConvertMaterialLayersToGraph();
	void SetMaterialGraph(const MaterialGraph& graph);
	void SetMaterialGraphLayout(const MaterialGraph& graph);
	void ClearMaterialGraph();
	void SetMaterialUseVM(bool use);
	void SetMaterialProgramSource(ParticleMaterial::ProgramSource source);


	// -------- Sprite 설정 -------
	void SetSpriteSettings(const SpriteSettings& settings);
	const SpriteSettings& GetSpriteSettings() const { return m_CpuRenderer.Settings(); }

	// -------- Module 설정 -------
	bool CanMoveModule(size_t moduleIndex, int direction) const;    // 같은 Stage 안에서만 이동
	void MoveModule(size_t moduleIndex, int direction);             // 재시작

	void AddModule(const ModuleDesc& module);
	void RemoveModule(size_t moduleIndex);

	// -------- GPU / CPU 변경 -------
	void SetBackend(EmitterBackend backend);                        // 재시작
	void Restart();

	// ---------- 조회 ----------
	EmitterBackend  Backend() const { return m_Backend; }
	bool            Ready() const { return m_Ready; }
	uint32_t        Capacity() const { return m_Capacity; }
	uint32_t        AliveCount() const;
	const std::string& LastError() const { return m_LastError; }
	const std::vector<std::string>& Warnings() const { return m_Compiled.warnings; }
	const UberEmitterData& Uber() const { return m_Uber; }
	bool			HasGeneratedProgram() const { return m_GpuProgram != nullptr; }

	// 지금 스프라이트를 그리는 픽셀 셰이더의 종류
	//   Preset      : 효과마다 고정된 셰이더 (Shaders/SpritePreset*PS.hlsl)       <- 머티리얼이 Preset 모드
	//   Generated   : 그래프에서 만든 셰이더 (MaterialGpuProgram)                 <- 머티리얼이 Custom 모드
	//   Interpreter : 기본 PS (EffectMode 분기 / VM). Material mode = Interpreted 이거나 생성에 실패했을 때
	enum class MaterialShaderKind { Interpreter, Preset, Generated };
	MaterialShaderKind MaterialShader() const
	{
		if (m_MaterialPreset)	return MaterialShaderKind::Preset;
		return m_MaterialProgram ? MaterialShaderKind::Generated : MaterialShaderKind::Interpreter;
	}
	bool			HasGeneratedMaterial() const { return m_MaterialProgram != nullptr; }
	const std::string& GeneratedMaterialSource() const;
	const std::string& GeneratedMaterialError() const { return m_MaterialProgramError; }

	static void     SetProgramCacheEnabled(bool enabled);
	static bool     ProgramCacheEnabled();
	static uint32_t ProgramCacheHits();

private:
	enum class Pending { None, Values, Preserve, Restart };     // 큰 값이 작은 값을 포함한다

	void Request(Pending pending);
	int  FindNeighbor(size_t moduleIndex, int direction) const;

	bool RebuildPreserve();
	bool RebuildRestart();
	bool BuildProgram(const CompiledEmitter& compiled, std::shared_ptr<EmitterGpuProgram>& out);
	void ShutdownActive();
	void RebuildUberData();

	bool NeedGeneratedProgram(const UberEmitterData& uber) const;
	void EnsureUberProgram();
	void SyncProgramWithMode();
	// 머티리얼 그래프가 바뀌었으면 (또는 모드가 바뀌었으면) 생성된 픽셀 셰이더를 맞춘다
	void SyncMaterialProgram();
	void SetMaterialProgram(std::shared_ptr<MaterialGpuProgram> program);
	// m_SpriteDesc 의 기본값을 렌더러 / 시뮬레이션에 넘긴다
	void ApplyRenderDefaults();

	std::vector<ModuleDesc> m_Modules;          // 원본. UI 가 직접 편집
	std::vector<ModuleDesc> m_AppliedModules;   // 마지막으로 적용에 성공한 스택 (실패 시 복구)
	RendererDesc            m_SpriteDesc = SpriteRenderer::Desc();   // 렌더러 바인딩 (UI 가 편집)
	RendererDesc            m_AppliedSpriteDesc = SpriteRenderer::Desc();
	CompiledEmitter         m_Compiled;         // m_AppliedModules 의 컴파일 결과
	UberEmitterData			m_Uber;
	ParticleMaterial        m_Material;         // Material

	EmitterBackend          m_Backend = EmitterBackend::Gpu;        // 요청된 백엔드
	EmitterBackend          m_ActiveBackend = EmitterBackend::Gpu;  // 실제로 실행 중인 백엔드
	uint32_t                m_Capacity = 0;
	uint32_t                m_AppliedCapacity = 0;  // 마지막으로 적용에 성공한 capacity (실패 시 복구)
	Pending                 m_Pending = Pending::None;
	bool                    m_Ready = false;
	std::string             m_LastError;

	EmitterInstance         m_Cpu;
	SpriteRenderer          m_CpuRenderer;

	ShaderCompiler                      m_ShaderCompiler;
	std::shared_ptr<EmitterGpuProgram>  m_GpuProgram;
	GpuEmitterInstance                  m_Gpu;
	SpriteRenderer                      m_GpuRenderer;

	std::shared_ptr<MaterialGpuProgram> m_MaterialProgram;              // Custom: 생성된 셰이더 (nullptr = 없음)
	bool                                m_MaterialPreset = false;       // Preset: 효과별 고정 셰이더로 그리는 중
	bool                                m_MaterialProgramTried = false; // 아래 revision 으로 만들어 봤는지 (실패해도 매 프레임 다시 하지 않는다)
	uint32_t                            m_MaterialProgramRevision = 0;
	std::string                         m_MaterialProgramError;
};
