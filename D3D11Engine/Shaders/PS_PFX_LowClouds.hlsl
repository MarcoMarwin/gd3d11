//--------------------------------------------------------------------------------------
// PostFX Low Clouds
// Applies world-coordinate low cloud fields in a separate stable pass so regular
// composition toggles do not recompile the expensive cloud raymarch code.
//--------------------------------------------------------------------------------------

#define ENABLE_LOW_CLOUDS 1
#include <AtmosphericScattering.h>
#include "DepthReconstruction.h"

cbuffer PFXBuffer : register( b0 )
{
    float4 HF_ProjParams;
    matrix HF_InvView;
    float3 HF_CameraPosition;
    float HF_FogHeight;

    float HF_HeightFalloff;
    float HF_GlobalDensity;
    float HF_WeightZNear;
    float HF_WeightZFar;

    float3 HF_FogColorMod;
    float HF_pad2;

    float2 HF_ProjAB;
    float2 HF_Pad3;
};

SamplerState SS_Linear : register( s0 );
Texture2D TX_Scene : register( t0 );
Texture2D TX_Depth : register( t1 );

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float3 VSPositionFromDepth( float depth, float2 vTexCoord )
{
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord, HF_ProjParams.xy );
}

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float4 color = TX_Scene.Sample( SS_Linear, Input.vTexcoord );
    if ( AC_SkyEffectsEnabled <= 0.0001f )
    {
        return color;
    }

    float expDepth = TX_Depth.Sample( SS_Linear, Input.vTexcoord ).r;
    float skyPixel = 1.0f - step( 0.00001f, expDepth );
    float3 worldPosition = VSPositionFromDepth( expDepth, Input.vTexcoord );
    worldPosition = mul( float4( worldPosition, 1.0f ), HF_InvView ).xyz;

    float cameraDistance = length( worldPosition - HF_CameraPosition );
    float nightTimeBlend = smoothstep( 0.0f, 1.0f, saturate( -AC_LightPos.y * 4.0f ) )
        * saturate( AC_EnableNightAtmosphere );

    float globalShadow = ComputeWorldLowCloudGlobalShadow( HF_FogHeight, nightTimeBlend );
    float surfaceShadow = (1.0f - skyPixel) * ComputeWorldLowCloudShadow( worldPosition, HF_FogHeight, nightTimeBlend );
    color.rgb *= 1.0f - saturate( globalShadow + surfaceShadow * 0.90f );

    float4 clouds = ComputeWorldLowCloudVolume(
        HF_CameraPosition,
        worldPosition,
        cameraDistance,
        skyPixel,
        HF_FogHeight,
        HF_FogColorMod,
        nightTimeBlend );

    color.rgb = lerp( color.rgb, clouds.rgb, clouds.a );
    return color;
}