#ifndef _HDR_H
#define _HDR_H

cbuffer HDR_Settings : register(b0)
{
    float HDR_MiddleGray;
    float HDR_LumWhite;
    float HDR_Threshold;
    float HDR_BloomStrength;
};

float GetToneMapExposure(Texture2D lumTex, SamplerState samplerState)
{
    const float fLumAvg = max(lumTex.SampleLevel(samplerState, float2(0.5f, 0.5f), 9).r, 0.001f);
    // Keep eye adaptation useful without letting a single bright or dark view dominate the image.
    return clamp(HDR_MiddleGray / fLumAvg, 0.75f, 4.0f);
}

float3 ToneMap_Simple(float3 vColor, Texture2D lumTex, SamplerState samplerState)
{
    vColor = max(vColor * GetToneMapExposure(lumTex, samplerState), 0.0f);
    vColor /= (1.0f + vColor);
    return vColor;
}

float3 LPMToneMap(float3 vColor, Texture2D lumTex, SamplerState samplerState) : COLOR
{
    const float3 lumaWeights = float3(0.2126f, 0.7152f, 0.0722f);
    const float whitePoint = max(HDR_LumWhite, 1.0f);

    float3 color = max(vColor * GetToneMapExposure(lumTex, samplerState), 0.0f);
    float luminanceIn = max(dot(color, lumaWeights), 0.0001f);
    float luminanceOut = (luminanceIn * (1.0f + luminanceIn / (whitePoint * whitePoint))) / (1.0f + luminanceIn);
    float3 mapped = color * (luminanceOut / luminanceIn);

    mapped = max(mapped, 0.0f);
    float peak = max(max(mapped.r, mapped.g), mapped.b);
    if (peak > 1.0f) {
        mapped /= peak;
    }

    // Stay linear here: this function is also used by the bloom threshold pass.
    return saturate(mapped);
}

#endif