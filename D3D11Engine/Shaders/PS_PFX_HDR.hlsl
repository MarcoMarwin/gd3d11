//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

#include <hdr.h>

SamplerState SS_Linear : register(s0);
Texture2D TX_Scene : register(t0);
Texture2D TX_Lum : register(t1);
Texture2D TX_Bloom : register(t2);

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float4 PSMain(PS_INPUT Input) : SV_TARGET
{
    float3 HDRColor = TX_Scene.Sample(SS_Linear, Input.vTexcoord).rgb;
#if USE_TONEMAP == 0
    float3 toneMapped = saturate(ToneMap_Simple(HDRColor, TX_Lum, SS_Linear));
#else
    float3 toneMapped = saturate(LPMToneMap(HDRColor, TX_Lum, SS_Linear));
#endif

    float3 bloom = TX_Bloom.Sample(SS_Linear, Input.vTexcoord).rgb * HDR_BloomStrength;
    float3 composed = saturate(toneMapped * (1.0f - bloom) + bloom);
#if USE_TONEMAP == 0
    composed = pow(composed, 1.8f);
#endif
    return float4(composed, 1.0f);
}
