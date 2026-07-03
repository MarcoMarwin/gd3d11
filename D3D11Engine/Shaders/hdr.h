#ifndef _HDR_H
#define _HDR_H

cbuffer HDR_Settings : register(b0)
{
    float HDR_MiddleGray;
    float HDR_LumWhite;
    float HDR_Threshold;
    float HDR_BloomStrength;
};

#if USE_TONEMAP == 1
#define FFX_GPU
#define FFX_HLSL
#include "FidelityFX/ffx_core.h"

cbuffer LPM_Constants : register(b1)
{
    uint4 LPM_Ctl[24];
};

FfxUInt32x4 LpmFilterCtl(FfxUInt32 index)
{
    return LPM_Ctl[index];
}

#include "FidelityFX/lpm/ffx_lpm.h"
#endif

float GetToneMapExposure(Texture2D lumTex, SamplerState samplerState, float middleGray, float minimumExposure)
{
    const float fLumAvg = max(lumTex.SampleLevel(samplerState, float2(0.5f, 0.5f), 9).r, 0.001f);
    return clamp(middleGray / fLumAvg, minimumExposure, 4.0f);
}

float3 ToneMap_Simple(float3 vColor, Texture2D lumTex, SamplerState samplerState)
{
    vColor = max(vColor * GetToneMapExposure(lumTex, samplerState, HDR_MiddleGray, 0.75f), 0.0f);
    vColor /= (1.0f + vColor);
    return vColor;
}

#if USE_TONEMAP == 1
float3 LPMToneMap(float3 vColor, Texture2D lumTex, SamplerState samplerState)
{
    FfxFloat32x3 color = max(vColor * GetToneMapExposure(lumTex, samplerState, 0.18f, 0.5f), 0.0f);
    LpmFilter(color.r, color.g, color.b, FFX_FALSE, LPM_CONFIG_709_709);
    return saturate(color);
}
#endif

#endif
