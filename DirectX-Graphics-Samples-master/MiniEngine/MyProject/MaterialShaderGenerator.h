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
//     예) const float2 v1 = v0 * float2(2.0f, 2.0f) + float2(0.0f, 0.0f) + float2(0.1f, 0.0f) * Time;
//         const float  v3 = 3.0f;
//         const float  v5 = v3 + v4.x;
//         const float4 v6 = v2 * v5;
// [3] 값은 전부 숫자로 직접 쓴다 (MaterialParams 를 읽지 않는다): Constant / UVTransform 노드의 값, 연결하지 않은 핀의 값
//     -> 값을 바꾸면 소스가 달라져서 다시 컴파일한다. 에디터는 드래그하는 동안 인터프리터로 미리 보고,
//        값을 놓았을 때 한 번만 만든다 (EmitterRuntime::SetMaterialGraph 의 liveEdit)
// [4] 소스가 같으면 같은 셰이더: 소스 문자열이 캐시의 키 (fileName 은 소스에 넣지 않는다)
GeneratedShader GenerateMaterialShader(const MaterialGraph& graph,
	const std::string& fileName = "Material.generated.hlsl");

// 이미 컴파일한 결과로 만들기 (ssaOk 여야 한다)
GeneratedShader GenerateMaterialShader(const MaterialCompileResult& compiled,
	const std::string& fileName = "Material.generated.hlsl");
