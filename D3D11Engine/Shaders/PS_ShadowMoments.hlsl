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
    depth = saturate(depth);
    float depth2 = depth * depth;
    float4 moments = float4(depth, depth2, depth2 * depth, depth2 * depth2);
    float4 optimized = mul(moments, float4x4(
        -2.07224649f,  13.79488572f,  0.105877704f,   9.79240621f,
        32.23703778f, -59.46839757f, -1.907746631f, -33.76521106f,
       -68.57107460f,  82.03597503f,  9.349655511f,  47.94560966f,
        39.37032741f, -35.36490326f, -6.654349074f, -23.97280482f));
    optimized.x += 0.0359558848f;
    return optimized;
}

float4 PSMain(PS_INPUT input) : SV_TARGET0
{
#if ALPHATEST == 1
    float alpha = TX_Texture0.Sample(SS_Linear, input.vTexcoord).a;
    DoAlphaTest(alpha);
#endif

    return PackShadowMoments(input.vPosition.z);
}