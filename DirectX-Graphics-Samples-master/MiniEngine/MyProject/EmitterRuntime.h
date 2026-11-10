#pragma once
#include "ModuleDesc.h"
#include "CompiledEmitter.h"
#include "EmitterInstance.h"
#include "EmitterGpuProgram.h"
#include "GpuEmitterInstance.h"
#include "ShaderCompiler.h"
#include "SpriteRenderer.h"

#include <memory>
#include <string>
#include <vector>

class GraphicsContext;
namespace Math { class Camera; }

enum class EmitterBackend { Cpu, Gpu };

// 모듈 스택(원본 데이터)과 실행 중인 백엔드 하나를 묶는다.
// 편집 함수는 요청만 남기고, 실제 적용은 프레임 시작의 ApplyPending() 에서 한다.
class EmitterRuntime
{
public:
	bool Initialize(const std::vector<ModuleDesc>& modules, uint32_t capacity, EmitterBackend backend);
	void Shutdown();

	void Tick(float deltaTime, const OwnerTransform& owner);
	void Tick(float deltaTime, const float3& ownerPosition);
	void Render(GraphicsContext& gfx, const Math::Camera& camera);
	void ApplyPending();

	// ---------- 편집 ----------
	std::vector<ModuleDesc>& EditModules() { return m_Modules; }

	void NotifyValuesChanged();     // ParamDesc::value 만 바뀜 (재컴파일 없음)
	void SetParamKind(size_t moduleIndex, size_t paramIndex, ParamKind kind);       // 파티클 유지
	void SetPerComponent(size_t moduleIndex, size_t paramIndex, bool perComponent); // 파티클 유지

	bool CanMoveModule(size_t moduleIndex, int direction) const;    // 같은 Stage 안에서만 이동
	void MoveModule(size_t moduleIndex, int direction);             // 재시작

	void SetBackend(EmitterBackend backend);                        // 재시작
	void Restart();

	// ---------- 조회 ----------
	EmitterBackend  Backend() const { return m_Backend; }
	bool            Ready() const { return m_Ready; }
	uint32_t        Capacity() const { return m_Capacity; }
	uint32_t        AliveCount() const;
	const std::string&              LastError() const { return m_LastError; }
	const std::vector<std::string>& Warnings() const { return m_Compiled.warnings; }

private:
	enum class Pending { None, Values, Preserve, Restart };     // 큰 값이 작은 값을 포함한다

	void Request(Pending pending);
	int  FindNeighbor(size_t moduleIndex, int direction) const;

	bool RebuildPreserve();
	bool RebuildRestart();
	bool BuildProgram(const CompiledEmitter& compiled, std::unique_ptr<EmitterGpuProgram>& out);
	void ShutdownActive();

	std::vector<ModuleDesc> m_Modules;          // 원본. UI 가 직접 편집
	std::vector<ModuleDesc> m_AppliedModules;   // 마지막으로 적용에 성공한 스택 (실패 시 복구)
	CompiledEmitter         m_Compiled;         // m_AppliedModules 의 컴파일 결과

	EmitterBackend          m_Backend = EmitterBackend::Gpu;        // 요청된 백엔드
	EmitterBackend          m_ActiveBackend = EmitterBackend::Gpu;  // 실제로 실행 중인 백엔드
	uint32_t                m_Capacity = 0;
	Pending                 m_Pending = Pending::None;
	bool                    m_Ready = false;
	std::string             m_LastError;

	EmitterInstance         m_Cpu;
	SpriteRenderer          m_CpuRenderer;

	ShaderCompiler                      m_ShaderCompiler;
	std::unique_ptr<EmitterGpuProgram>  m_GpuProgram;
	GpuEmitterInstance                  m_Gpu;
	SpriteRenderer                      m_GpuRenderer;
};
