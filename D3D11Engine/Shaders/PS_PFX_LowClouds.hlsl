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
    float cloudLightOcclusion = saturate( globalShadow + surfaceShadow * 0.88f );
    float sunWeight = saturate( AC_SunVisibility ) * smoothstep( 0.04f, 0.42f, AC_LightPos.y );
    float moonWeight = saturate( AC_MoonVisibility ) * saturate( AC_EnableNightAtmosphere ) * smoothstep( 0.02f, 0.34f, AC_MoonPos.y ) * 0.34f;
    float directLightWeight = saturate( sunWeight + moonWeight );
    float sceneLuma = dot( color.rgb, float3( 0.299f, 0.587f, 0.114f ) );
    float directLitMask = smoothstep( 0.12f, 0.58f, sceneLuma ) * directLightWeight * (1.0f - skyPixel);
    float shadowAttenuation = saturate( cloudLightOcclusion * lerp( 0.74f, 1.34f, directLitMask ) );
    color.rgb *= 1.0f - shadowAttenuation;

    float4 clouds = ComputeWorldLowCloudVolume(
        HF_CameraPosition,
        worldPosition,
        cameraDistance,
        skyPixel,
        HF_FogHeight,
        HF_FogColorMod,
        nightTimeBlend );

    float3 viewDir = normalize( worldPosition - HF_CameraPosition );
    float3 sunDir = normalize( lerp( float3( -0.25f, 0.72f, 0.18f ), AC_LightPos, saturate( abs( AC_LightPos.y ) + 0.12f ) ) );
    float3 moonDir = normalize( lerp( float3( 0.22f, 0.64f, -0.28f ), AC_MoonPos, saturate( abs( AC_MoonPos.y ) + 0.12f ) ) );
    float moonDiskWeight = saturate( AC_MoonVisibility ) * saturate( AC_EnableNightAtmosphere ) * smoothstep( 0.02f, 0.34f, AC_MoonPos.y );
    float sunAlignment = dot( viewDir, sunDir );
    float moonAlignment = dot( viewDir, moonDir );
    float sunCore = smoothstep( 0.99920f, 0.99986f, sunAlignment ) * sunWeight * skyPixel;
    float sunHalo = smoothstep( 0.99200f, 0.99860f, sunAlignment ) * sunWeight * skyPixel;
    float moonCore = smoothstep( 0.99935f, 0.99988f, moonAlignment ) * moonDiskWeight * skyPixel;
    float moonHalo = smoothstep( 0.99500f, 0.99900f, moonAlignment ) * moonDiskWeight * skyPixel;
    float lightDiskMask = saturate( sunCore + sunHalo * 0.42f + moonCore * 0.92f + moonHalo * 0.24f );
    float cloudCoverAtLight = saturate( clouds.a * 1.58f + globalShadow * 0.86f );
    float lightCoreMask = saturate( sunCore + moonCore );
    float lightDiskOcclusion = saturate( lightDiskMask * cloudCoverAtLight );
    float3 occludedScene = color.rgb * ( 1.0f - lightDiskOcclusion * lerp( 0.72f, 0.985f, lightCoreMask ) );

    color.rgb = lerp( occludedScene, clouds.rgb, clouds.a );
    return color;
}
