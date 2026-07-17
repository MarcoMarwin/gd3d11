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

struct PS_OUTPUT
{
    float4 Clouds : SV_TARGET0;
    float Depth : SV_TARGET1;
};

float3 VSPositionFromDepth( float depth, float2 vTexCoord )
{
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord, HF_ProjParams.xy );
}

float LoadClosestDepth2x2( float2 texcoord )
{
    uint depthWidth;
    uint depthHeight;
    TX_Depth.GetDimensions( depthWidth, depthHeight );

    int2 depthSize = max( int2( depthWidth, depthHeight ), int2( 1, 1 ) );
    int2 maxPixel = depthSize - int2( 1, 1 );
    int2 upperPixel = int2( floor(
        texcoord * float2( depthSize ) + 0.5f ) );
    int2 basePixel = upperPixel - int2( 1, 1 );

    int2 pixel00 = clamp( basePixel, int2( 0, 0 ), maxPixel );
    int2 pixel10 = clamp( basePixel + int2( 1, 0 ), int2( 0, 0 ), maxPixel );
    int2 pixel01 = clamp( basePixel + int2( 0, 1 ), int2( 0, 0 ), maxPixel );
    int2 pixel11 = clamp( basePixel + int2( 1, 1 ), int2( 0, 0 ), maxPixel );

    float depth00 = TX_Depth.Load( int3( pixel00, 0 ) ).r;
    float depth10 = TX_Depth.Load( int3( pixel10, 0 ) ).r;
    float depth01 = TX_Depth.Load( int3( pixel01, 0 ) ).r;
    float depth11 = TX_Depth.Load( int3( pixel11, 0 ) ).r;

    // Reversed-Z: the largest value is the closest surface in this 2x2 footprint.
    return max( max( depth00, depth10 ), max( depth01, depth11 ) );
}

PS_OUTPUT PSMain( PS_INPUT Input )
{
    float expDepth = LoadClosestDepth2x2( Input.vTexcoord );
    float skyPixel = 1.0f - step( 0.00001f, expDepth );
    float3 worldPosition = VSPositionFromDepth( expDepth, Input.vTexcoord );
    worldPosition = mul( float4( worldPosition, 1.0f ), HF_InvView ).xyz;

    float cameraDistance = length( worldPosition - HF_CameraPosition );
    float nightTimeBlend = smoothstep( 0.0f, 1.0f, saturate( -AC_LightPos.y * 4.0f ) )
        * saturate( AC_EnableNightAtmosphere );

    float globalShadow = 0.0f;
    float sunWeight = saturate( AC_SunVisibility ) * smoothstep( 0.04f, 0.42f, AC_LightPos.y );

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
    float nightFogBrightness = lerp( 1.0f, max( 0.0f, AC_NightFogBrightness ), saturate( AC_EnableNightAtmosphere ) );
    float3 nightRainVeilColor = float3( 0.12f, 0.18f, 0.27f ) * nightFogBrightness / 2.5f;
    float rainVeil = saturate( AC_RainFXWeight ) * lerp( 0.045f, 0.30f, nightTimeBlend );
    float veilDistance = SmootherStep01( saturate( ( cameraDistance - 3500.0f ) / 52000.0f ) );
    float veilAmount = rainVeil * lerp( 0.45f, 1.0f, skyPixel ) * veilDistance;
    clouds.rgb = lerp( clouds.rgb, nightRainVeilColor, veilAmount );
    clouds.a *= 1.0f - veilAmount * 0.34f;

    float lightDiskMask = saturate( sunCore + sunHalo * 0.42f + moonCore * 0.92f + moonHalo * 0.24f );
    float cloudCoverAtLight = saturate( clouds.a * 1.90f + globalShadow * 0.86f );
    float lightCoreMask = saturate( sunCore + moonCore );
    float lightDiskOcclusion = saturate( lightDiskMask * cloudCoverAtLight ) * lerp( 0.78f, 0.995f, lightCoreMask );

    float layerAlpha = saturate( clouds.a + lightDiskOcclusion * ( 1.0f - clouds.a ) );

    PS_OUTPUT output;
    output.Clouds = float4( clouds.rgb * clouds.a, layerAlpha );
    output.Depth = expDepth;
    return output;
}
