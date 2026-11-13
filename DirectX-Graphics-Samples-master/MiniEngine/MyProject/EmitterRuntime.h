#pragma once
#include "ModuleDesc.h"
#include "CompiledEmitter.h"
#include "EmitterInstance.h"
#include "EmitterGpuProgram.h"
#include "GpuEmitterInstance.h"
#include "ShaderCompiler.h"
#include "SpriteRenderer.h"
#include "UberEmitterData.h"

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
	void Shutdown();

	void Tick(ComputeContext& ctx, float deltaTime, const OwnerTransform& owner);
	void Tick(ComputeContext& ctx, float deltaTime, const float3& ownerPosition);
	void Render(GraphicsContext& gfx, const Math::Camera& camera);
	
	GpuEmitterInstance* StagedGpu() { return (m_Ready && m_ActiveBackend == EmitterBackend::Gpu) ? &m_Gpu : nullptr; }
	
	void OnSubmitted(uint64_t fence);
	void ApplyPending();

	// ---------- 편집 ----------
	std::vector<ModuleDesc>& EditModules() { return m_Modules; }

	void NotifyValuesChanged();     // ParamDesc::value 만 바뀜 (재컴파일 없음)
	void SetParamKind(size_t moduleIndex, size_t paramIndex, ParamKind kind);       // 파티클 유지
	void SetPerComponent(size_t moduleIndex, size_t paramIndex, bool perComponent); // 파티클 유지

	// -------- 렌더러 바인딩 -------
	const RendererDesc& SpriteBindings() const { return m_SpriteDesc; }
	// 연결할 속성 변경 (빈 문자열 = 항상 기본값). Trim 대상이 바뀔 수 있어 재시작
	void SetBindingAttribute(size_t bindingIndex, const std::string& attribute);
	// 기본값만 변경. 재컴파일 없이 바로 반영
	void SetBindingDefault(size_t bindingIndex, const std::vector<uint8_t>& value);
	// 이 칸에 연결할 수 있는 속성 목록 (적용된 모듈이 쓰는 같은 타입의 Particles 속성 + Previous / Initial)
	std::vector<std::string> BindableAttributes(const TypeInfo* type) const;

	// -------- 스프라이트 텍스처 -------
	// 빈 문자열 = 기본 원형. 재컴파일 / 재시작 없이 바로 반영. 실패하면 magenta 로 그려지고 false
	bool SetSpriteTexture(const std::string& path);
	const std::string& SpriteTexture() const { return m_SpriteTexture; }

	void SetSpriteSettings(const SpriteSettings& settings);
	const SpriteSettings& GetSpriteSettings() const { return m_CpuRenderer.Settings(); }

	bool CanMoveModule(size_t moduleIndex, int direction) const;    // 같은 Stage 안에서만 이동
	void MoveModule(size_t moduleIndex, int direction);             // 재시작

	void AddModule(const ModuleDesc& module);
	void RemoveModule(size_t moduleIndex);

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


	std::vector<ModuleDesc> m_Modules;          // 원본. UI 가 직접 편집
	std::vector<ModuleDesc> m_AppliedModules;   // 마지막으로 적용에 성공한 스택 (실패 시 복구)
	RendererDesc            m_SpriteDesc = SpriteRenderer::Desc();   // 렌더러 바인딩 (UI 가 편집)
	RendererDesc            m_AppliedSpriteDesc = SpriteRenderer::Desc();
	CompiledEmitter         m_Compiled;         // m_AppliedModules 의 컴파일 결과
	UberEmitterData			m_Uber;
	std::string             m_SpriteTexture;    // 렌더러가 재시작해도 유지 (렌더러 쪽 캐시)

	EmitterBackend          m_Backend = EmitterBackend::Gpu;        // 요청된 백엔드
	EmitterBackend          m_ActiveBackend = EmitterBackend::Gpu;  // 실제로 실행 중인 백엔드
	uint32_t                m_Capacity = 0;
	Pending                 m_Pending = Pending::None;
	bool                    m_Ready = false;
	std::string             m_LastError;

	EmitterInstance         m_Cpu;
	SpriteRenderer          m_CpuRenderer;

	ShaderCompiler                      m_ShaderCompiler;
	std::shared_ptr<EmitterGpuProgram>  m_GpuProgram;
	GpuEmitterInstance                  m_Gpu;
	SpriteRenderer                      m_GpuRenderer;
};
