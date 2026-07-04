#ifndef _HDR_H
#define _HDR_H

cbuffer HDR_Settings : register(b0)
{
    float HDR_MiddleGray;
    float HDR_LumWhite;
    float HDR_Threshold;
    float HDR_BloomStrength;
    float HDR_ToneMapStrength;
    float3 HDR_Pad;
};

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
float GetToneMapExposure(Texture2D lumTex, SamplerState samplerState, float middleGray, float minimumExposure, float maximumExposure)
{
    const float fLumAvg = max(lumTex.SampleLevel(samplerState, float2(0.5f, 0.5f), 9).r, 0.001f);
    return clamp(middleGray / fLumAvg, minimumExposure, maximumExposure);
}

float HDRToneMapBlend()
{
    float legacyToneMapStrength = HDR_ToneMapStrength * 7.5f;
    return saturate((legacyToneMapStrength - 1.0f) / 9.0f);
}

float3 LPMToneMap(float3 vColor, Texture2D lumTex, SamplerState samplerState)
{
    FfxFloat32x3 color = max(vColor * GetToneMapExposure(lumTex, samplerState, 0.18f, 0.75f, 2.25f), 0.0f);
    LpmFilter(color.r, color.g, color.b,
        FFX_TRUE, FFX_FALSE, FFX_FALSE, FFX_FALSE, FFX_FALSE, FFX_FALSE);
    return lerp(saturate(vColor), saturate(color), HDRToneMapBlend());
}
#endif
