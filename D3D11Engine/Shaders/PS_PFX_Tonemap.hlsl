//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

#include <hdr.h>

SamplerState SS_Linear : register(s0);
Texture2D TX_Scene : register(t0);
Texture2D TX_Lum : register(t1);

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float4 PSMain(PS_INPUT Input) : SV_TARGET
{
    float3 HDRColor = TX_Scene.Sample(SS_Linear, Input.vTexcoord).rgb;
    float3 toneMapped = LPMToneMap(HDRColor, TX_Lum, SS_Linear);

    toneMapped = max(toneMapped - HDR_Threshold, 0.0f);
    return float4(toneMapped, 1.0f);
}