#include "pch.h"
#include "ParticleMaterial.h"

#include <cstring>

namespace
{
    const char* const kOpNames[] = { "Off", "Multiply", "Add", "AlphaMask", "Erode", "Distort" };
    const char* const kUVNames[] = { "Quad", "SubUV" };
    const char* const kChannelNames[] = { "RGBA", "R", "G", "B", "A", "Luminance" };
    const char* const kParamNames[] = { "Constant", "Param0", "Param1" };
    static_assert(_countof(kOpNames) == size_t(SpriteLayerOp::Count), "SpriteLayerOp 이름 표");
    static_assert(_countof(kUVNames) == size_t(SpriteLayerUV::Count), "SpriteLayerUV 이름 표");
    static_assert(_countof(kChannelNames) == size_t(SpriteLayerChannel::Count), "SpriteLayerChannel 이름 표");
    static_assert(_countof(kParamNames) == size_t(SpriteLayerParam::Count), "SpriteLayerParam 이름 표");

    template<size_t N>
    const char* NameOf(const char* const (&names)[N], uint32_t v) { return v < N ? names[v] : "?"; }

    float s_Time = 0.f;
}

const char* SpriteLayerOpName(SpriteLayerOp v) { return NameOf(kOpNames, uint32_t(v)); }
const char* SpriteLayerUVName(SpriteLayerUV v) { return NameOf(kUVNames, uint32_t(v)); }
const char* SpriteLayerChannelName(SpriteLayerChannel v) { return NameOf(kChannelNames, uint32_t(v)); }
const char* SpriteLayerParamName(SpriteLayerParam v) { return NameOf(kParamNames, uint32_t(v)); }

SpriteLayer DefaultSpriteLayer(SpriteLayerOp op)
{
    SpriteLayer l;
    l.op = op;
    switch (op)
    {
    case SpriteLayerOp::Multiply:
        l.srgb = true;                                  // 색 텍스처
        break;
    case SpriteLayerOp::Add:
        l.srgb = true;
        l.intensity = 2.f;
        break;
    case SpriteLayerOp::AlphaMask:
        l.channel = SpriteLayerChannel::R;
        break;
    case SpriteLayerOp::Erode:
        // 디졸브: 수명 후반에 노이즈 모양으로 사라지며 경계가 탄다 (MaterialParam0 <- NormalizedAge 로 연결)
        l.channel = SpriteLayerChannel::R;
        l.param = SpriteLayerParam::Param0;
        l.paramRange = { 0.5f, 1.f };
        l.tint = { 1.f, 0.35f, 0.05f };
        l.intensity = 4.f;
        break;
    case SpriteLayerOp::Distort:
        l.channel = SpriteLayerChannel::RGBA;           // rg = 방향
        l.intensity = 0.03f;
        l.wrap = true;
        l.scroll = { 0.f, 0.2f };
        break;
    default:
        break;
    }
    return l;
}

uint32_t PackSpriteLayers(const SpriteLayer (&layers)[kMaxSpriteLayers], const bool (&hasTexture)[kMaxSpriteLayers],
    SpriteLayerGpu (&out)[kMaxSpriteLayers], uint32_t (&sourceIndex)[kMaxSpriteLayers])
{
    std::memset(out, 0, sizeof(out));
    uint32_t n = 0;
    for (uint32_t i = 0; i < kMaxSpriteLayers; ++i)
    {
        const SpriteLayer& l = layers[i];
        if (l.op == SpriteLayerOp::Off || l.op >= SpriteLayerOp::Count || !hasTexture[i])
            continue;

        SpriteLayerGpu& g = out[n];
        g.op = uint32_t(l.op);
        g.uv = uint32_t(l.uv);
        g.channel = uint32_t(l.channel);
        g.flags = (l.wrap ? kSpriteLayerWrap : 0u) | (l.maskBySprite ? kSpriteLayerMaskBySprite : 0u);
        g.tiling[0] = l.tiling.x;       g.tiling[1] = l.tiling.y;
        g.offset[0] = l.offset.x;       g.offset[1] = l.offset.y;
        g.scroll[0] = l.scroll.x;       g.scroll[1] = l.scroll.y;
        g.paramRange[0] = l.paramRange.x; g.paramRange[1] = l.paramRange.y;
        g.tint[0] = l.tint.x; g.tint[1] = l.tint.y; g.tint[2] = l.tint.z;
        g.intensity = l.intensity;
        g.param = uint32_t(l.param);
        g.textureIndex = n;
        g.edgeWidth = l.edgeWidth;
        g.softness = l.softness;
        sourceIndex[n] = i;
        ++n;
    }
    for (uint32_t k = n; k < kMaxSpriteLayers; ++k)
        sourceIndex[k] = 0;
    return n;
}

void  ParticleMaterial::SetTime(float seconds) { s_Time = seconds; }
float ParticleMaterial::Time() { return s_Time; }
