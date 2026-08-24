// Build-213 Forward+ pre-light support: reconstruct a view-space normal from
// the depth prepass so XeGTAO can run before the lit geometry pass.
#include "DS_Defines.h"
#include "DepthReconstruction.h"

cbuffer DS_ScreenQuadConstantBuffer : register( b0 )
{
    float4 SQ_ProjParams;
    matrix SQ_InvView;
    matrix SQ_View;
    matrix SQ_RainViewProj;
    float3 SQ_LightDirectionVS;
    float SQ_ShadowmapSize;
    float4 SQ_LightColor;
    matrix SQ_ShadowViewProj[4];
    float SQ_ShadowStrength;
    float SQ_ShadowAOStrength;
    float SQ_WorldAOStrength;
    float SQ_ShadowSoftness;
    uint SQ_FrameIndex;
    float2 SQ_JitterOffset;
    float SQ_LightSize;
    float4 SQ_CascadeAtlasRect[4];
    float4 SQ_CascadeLightDirectionWS[4];
    float4 SQ_ShadowRuntimeParams;
};

SamplerState SS_Linear : register( s0 );
Texture2D TX_Depth : register( t2 );

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay   : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float3 ViewPosition( float2 uv )
{
    float depth = TX_Depth.SampleLevel( SS_Linear, uv, 0 ).r;
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, uv - SQ_JitterOffset, SQ_ProjParams.xy );
}

float2 PSMain( PS_INPUT input ) : SV_TARGET
{
    float2 uv = input.vTexcoord;
    float depth = TX_Depth.SampleLevel( SS_Linear, uv, 0 ).r;
    if ( !(depth > 0.0f) )
        return EncodeNormalGBuffer( float3( 0.0f, 0.0f, 1.0f ) );

    float3 center = ViewPosition( uv );
    float3 dx = ddx( center );
    float3 dy = ddy( center );
    float3 normal = normalize( cross( dx, dy ) );
    return EncodeNormalGBuffer( normal );
}
