#include <AtmosphericScattering.h>
#include <FFFog.h>
#include <DS_Defines.h>
#include <Toolbox.h>

cbuffer MI_MaterialInfo : register( b2 )
{
    float MI_SpecularIntensity;
    float MI_SpecularPower;
    float MI_NormalmapStrength;
    float MI_ParallaxOcclusionStrength;
    float4 MI_Color;
}

cbuffer DIST_Distance : register( b3 )
{
    float DIST_DrawDistance;
    float3 DIST_Pad;
}

SamplerState SS_Linear : register( s0 );
Texture2D TX_Texture0 : register( t0 );

struct PS_INPUT
{
    float2 vTexcoord     : TEXCOORD0;
    float2 vTexcoord2    : TEXCOORD1;
    float4 vDiffuse      : TEXCOORD2;
    float3 vNormalVS     : TEXCOORD4;
    float3 vViewPosition : TEXCOORD5;
    float4 vCurrClipPos  : TEXCOORD6;
    float4 vPrevClipPos  : TEXCOORD7;
    float4 vPosition     : SV_POSITION;
};

float4 PackShadowMoments(float depth)
{
    // Improved 64-bit MSM: signed depth plus the sparse quantization transform
    // from Peters et al. reduces quantization noise and costs less than 4x4 mul.
    float z = saturate(depth) * 2.0f - 1.0f;
    float z2 = z * z;
    float z3 = z2 * z;
    float z4 = z2 * z2;
    return float4(
        0.5f + 1.5f * z - 2.0f * z3,
        4.0f * z2 - 4.0f * z4,
        0.5f + 0.8660254038f * z - 0.3849001795f * z3,
        0.5f * z2 + 0.5f * z4);
}

float4 PSMain(PS_INPUT input) : SV_TARGET0
{
#if ALPHATEST == 1
    float alpha = TX_Texture0.Sample(SS_Linear, input.vTexcoord).a;
    DoAlphaTest(alpha);
#endif

    return PackShadowMoments(input.vPosition.z);
}