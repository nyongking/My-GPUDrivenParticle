#pragma once
#include "MaterialGraph.h"
#include "ShaderGenerator.h"

#include <string>

// MaterialGraph -> 픽셀 셰이더 HLSL 문자열 (Generated 방식)
//
// ------ 만들어지는 셰이더의 규칙 ------
// [1] 선언은 전부 Shaders/SpriteMaterialCommon.hlsli 에 있다 (ParticleSpritePS.hlsl 과 같은 루트 시그니처)
// [2] 명령어 하나 = 변수 하나. MaterialVM.hlsli 의 case 와 같은 계산이지만 (VM 과 그림이 같아야 한다)
//     VM 처럼 전부 float4 로 두지 않고, 값의 성분 수에 맞는 타입(float / float2 / float3 / float4)을 쓴다
//     타입은 MaterialCompileResult::ssaInfo (노드의 출력 너비 + 연결의 swizzle / width) 에서 온다
//     예) const float2 v1 = v0 * MaterialParams[0].xy + ...;
//         const float  v5 = v3.x + v4.x;
//         const float4 v6 = v2 * v5;
// [3] 노드 값(Constant / UVTransform)은 MaterialParams[k] 로 읽는다 -> 값만 바꾸면 소스가 같다 (재컴파일 없음)
//     빈 핀 기본값은 바뀔 일이 없으므로 숫자로 직접 쓴다
// [4] 소스가 같으면 같은 셰이더: 소스 문자열이 캐시의 키 (fileName 은 소스에 넣지 않는다)
GeneratedShader GenerateMaterialShader(const MaterialGraph& graph,
	const std::string& fileName = "Material.generated.hlsl");

// 이미 컴파일한 결과로 만들기 (ssaOk 여야 한다)
GeneratedShader GenerateMaterialShader(const MaterialCompileResult& compiled,
	const std::string& fileName = "Material.generated.hlsl");
