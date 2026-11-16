// 스프라이트 텍스처 알파의 해석 방법
// C++ SpriteTextureAlpha 와 같은 번호
#ifndef SPRITE_ALPHA_HLSLI
#define SPRITE_ALPHA_HLSLI

#define ALPHA_STRAIGHT       0   // 일반 PNG
#define ALPHA_PREMULTIPLIED  1   // RGB 에 알파가 이미 곱해짐
#define ALPHA_FROM_LUMINANCE 2   // 알파 없음, 검정 = 투명

// 결과는 premultiplied (BlendPreMultiplied: ONE, INV_SRC_ALPHA)
float4 ResolveSpriteColor(float4 color, float4 tex, uint alphaMode, uint blendMode)
{
    float3 texPremul;
    if (alphaMode == ALPHA_FROM_LUMINANCE)
    {
        tex.a = max(tex.r, max(tex.g, tex.b));
        texPremul = tex.rgb;
    }
    else if (alphaMode == ALPHA_PREMULTIPLIED)
    {
        texPremul = tex.rgb;
    }
    else
    {
        texPremul = tex.rgb * tex.a;
    }

    const float a = color.a * tex.a;
    if (blendMode == 1)
        return float4(color.rgb * color.a * texPremul, 0.f);
    else
        return float4(color.rgb * color.a * texPremul, a);
}

#endif // SPRITE_ALPHA_HLSLI