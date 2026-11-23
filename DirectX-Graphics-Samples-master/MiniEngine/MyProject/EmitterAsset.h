#pragma once
// 이미터 하나를 데이터(JSON 파일)로 저장 / 불러오기
//
//   저장하는 것 : 모듈 스택 (모듈 이름 + 입력값, 순서 그대로), 렌더러 바인딩 (연결 속성 + 기본값),
//                 텍스처 경로, 스프라이트 설정 (텍스처 알파 / SubUV / 블렌드 / 빌보드), capacity, 백엔드
//   저장 안 함  : 모듈의 HLSL / CPU 함수 / reads / writes / Stage
//                 -> 불러올 때 ModuleLibrary 의 MakeDefaultModule(이름) 으로 다시 만들고 입력값만 덮어쓴다
//                    (모듈 코드를 고치면 기존 파일도 새 코드로 돈다)
//
//   파일 : Assets/Emitters/<이름>.emitter.json (UTF-8)

#include "EmitterRuntime.h"
#include "MaterialGraph.h"

#include <cstdint>
#include <string>
#include <vector>

struct EmitterAsset
{
	static const uint32_t kVersion = 1;

	std::string             name;
	uint32_t                capacity = 0;                    // 0 = 적용할 때 지금 값 유지
	EmitterBackend          backend = EmitterBackend::Gpu;
	std::vector<ModuleDesc> modules;                         // 실행 순서 그대로
	RendererDesc            sprite = SpriteRenderer::Desc(); // 바인딩 (칸 순서 = Desc())
	
	MaterialTexture			textures[ParticleMaterial::kNumTextureSlot];
	float4					materialParams[ParticleMaterial::kMaxParams];	// PS Param(k). 파일에 없으면 DefaultParam
	std::vector<ParticleMaterial::Effect>	materialLayers;
	bool									hasMaterialGraph = false;
	MaterialGraph							materialGraph;

	SpriteSettings          settings;

	EmitterAsset();
};

// 지금 편집 중인 상태를 에셋으로 (UI 가 보고 있는 값 = 다음 ApplyPending 에서 적용될 값)
EmitterAsset CaptureEmitterAsset(const EmitterRuntime& runtime, const std::string& name);

bool         ApplyEmitterAsset(EmitterRuntime& runtime, const EmitterAsset& asset, std::vector<std::string>* failedTextures);
// 아직 시작하지 않은 런타임을 에셋으로 시작 (재시작 없이 한 번에). capacity == 0 이면 에셋 값
//   실패하면 false (runtime.LastError()). 텍스처를 못 읽는 것은 출력 창 경고만 (magenta 로 그려짐)
bool         InitializeEmitterFromAsset(EmitterRuntime& runtime, const EmitterAsset& asset, uint32_t capacity);

// 문자열 단위. 실패(파일 형식 / 모르는 모듈)면 out 은 그대로, 고칠 수 있는 문제는 warnings 에 남기고 계속
std::string EmitterAssetToJson(const EmitterAsset& asset);
bool        EmitterAssetFromJson(const std::string& text, EmitterAsset& out, std::string& error, std::vector<std::string>& warnings);

// 파일 단위. 저장은 임시 파일에 쓴 뒤 교체한다 (쓰다가 실패해도 기존 파일이 남음)
bool SaveEmitterAsset(const std::string& path, const EmitterAsset& asset, std::string& error);
bool LoadEmitterAsset(const std::string& path, EmitterAsset& out, std::string& error, std::vector<std::string>& warnings);

const char*              EmitterAssetDirectory();          // "Assets/Emitters/"
const char*              EmitterAssetExtension();          // ".emitter.json"
std::string              EmitterAssetPath(const std::string& name);
bool                     IsValidEmitterAssetName(const std::string& name);   // 영문 / 숫자 / _ - . 만
std::vector<std::string> ListEmitterAssets();              // 폴더 안의 에셋 이름 (확장자 제외, 정렬)
