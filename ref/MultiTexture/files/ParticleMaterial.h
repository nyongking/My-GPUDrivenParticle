#pragma once
// 스프라이트 텍스처 레이어 = 베이스 텍스처 위에 텍스처를 최대 4 장까지 데이터로 쌓는다
//
//   레이어마다 : 텍스처 (sRGB / 선형) + UV (사각형 또는 SubUV 칸, 타일링 / 오프셋 / 스크롤 / 반복)
//                + 채널 + 연산 + 세기
//   연산       : Multiply  색 곱하기          (예: 색 변화 텍스처, 디테일)
//                Add       발광 더하기        (예: 불씨 / 하이라이트)
//                AlphaMask 불투명도 곱하기    (예: 가장자리 마스크)
//                Erode     잘라내기 + 경계 발광 (예: 디졸브)
//                Distort   UV 왜곡           (베이스와 뒤 레이어의 uv 를 민다. 예: 열기 / 일렁임)
//   세기       : 상수, 또는 파티클별 MaterialParam0 / 1 바인딩을 범위로 0~1 로 바꾼 값
//                (예: MaterialParam0 <- Particles.NormalizedAge -> 수명에 따라 디졸브 / 발광)
//
//   PS 는 하나 (SpriteMaterial.hlsli). 레이어 구성이 달라도 PSO 가 같아서 전체 정렬 경로의 Draw 1번이 유지된다

#include "AttributeType.h"

#include <cstdint>

constexpr uint32_t kMaxSpriteLayers = 4;   // HLSL MAX_SPRITE_LAYERS

// 번호 = HLSL LAYER_* / LAYER_UV_* / CHANNEL_* / PARAM_*
enum class SpriteLayerOp : uint32_t { Off, Multiply, Add, AlphaMask, Erode, Distort, Count };
enum class SpriteLayerUV : uint32_t { Quad, SubUV, Count };                      // 사각형 0~1 / 베이스와 같은 SubUV 칸
enum class SpriteLayerChannel : uint32_t { RGBA, R, G, B, A, Luminance, Count }; // 한 채널을 고르면 4 성분에 복사
enum class SpriteLayerParam : uint32_t { Constant, Param0, Param1, Count };

// UI / 저장 파일에 쓰는 이름 (바꾸면 기존 파일을 못 읽는다)
const char* SpriteLayerOpName(SpriteLayerOp v);
const char* SpriteLayerUVName(SpriteLayerUV v);
const char* SpriteLayerChannelName(SpriteLayerChannel v);
const char* SpriteLayerParamName(SpriteLayerParam v);

struct SpriteLayer
{
    SpriteLayerOp      op = SpriteLayerOp::Off;
    bool               srgb = false;                     // 색 텍스처 = true, 마스크 / 노이즈 = false (경로는 SetLayerTexture)
    SpriteLayerUV      uv = SpriteLayerUV::Quad;
    SpriteLayerChannel channel = SpriteLayerChannel::RGBA;
    float2             tiling = { 1.f, 1.f };
    float2             offset = { 0.f, 0.f };
    float2             scroll = { 0.f, 0.f };            // uv / 초 (ParticleMaterial::Time 기준)
    bool               wrap = false;                     // 반복 (타일링 / 스크롤 할 때)
    bool               maskBySprite = true;              // Add: 베이스 모양 안에만 더한다
    float3             tint = { 1.f, 1.f, 1.f };         // Multiply / Add 색, Erode 경계색
    float              intensity = 1.f;                  // Multiply / AlphaMask = 섞는 비율, Add / Erode = 밝기, Distort = uv 이동량
    SpriteLayerParam   param = SpriteLayerParam::Constant;
    float2             paramRange = { 1.f, 1.f };        // Constant: x = 값 / Param: (시작, 끝) -> 0~1 (끝 < 시작 이면 거꾸로)
    float              edgeWidth = 0.05f;                // Erode: 경계 발광 폭 (마스크 값 단위)
    float              softness = 0.02f;                 // Erode: 잘리는 곳의 부드러운 폭
};

// 연산마다 쓸 만한 시작값 (UI 에서 연산을 처음 고를 때)
SpriteLayer DefaultSpriteLayer(SpriteLayerOp op);

// HLSL SpriteLayerGpu 와 같은 배치 (80 B = 16 B 줄 5개. cbuffer 배열 / StructuredBuffer 어느 쪽이든 같은 배치)
struct SpriteLayerGpu
{
    uint32_t op, uv, channel, flags;          // 0
    float    tiling[2], offset[2];            // 16
    float    scroll[2], paramRange[2];        // 32
    float    tint[3], intensity;              // 48
    uint32_t param, textureIndex;             // 64  textureIndex: 이미터별 경로 = 레이어 칸, 전체 정렬 = 텍스처 표 번호
    float    edgeWidth, softness;             // 72
};
static_assert(sizeof(SpriteLayerGpu) == 80, "SpriteLayerGpu 80 B");

constexpr uint32_t kSpriteLayerWrap = 1u;          // HLSL LAYER_FLAG_WRAP
constexpr uint32_t kSpriteLayerMaskBySprite = 2u;  // HLSL LAYER_FLAG_MASK_BY_SPRITE

// 꺼진 레이어 / 텍스처가 없는 레이어를 빼고 앞으로 모은다. 반환 = 개수
//   sourceIndex[k] = 원래 레이어 번호 (텍스처를 찾을 때). textureIndex 는 호출한 쪽이 채운다
uint32_t PackSpriteLayers(const SpriteLayer (&layers)[kMaxSpriteLayers], const bool (&hasTexture)[kMaxSpriteLayers],
    SpriteLayerGpu (&out)[kMaxSpriteLayers], uint32_t (&sourceIndex)[kMaxSpriteLayers]);

// 레이어 스크롤용 시간 (초). Main 이 매 프레임 넣는다 (Time stop 이면 멈춤)
namespace ParticleMaterial
{
    void  SetTime(float seconds);
    float Time();
}
