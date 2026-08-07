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
    float HF_FogOverride;
    float2 HF_ProjAB;
    float2 HF_Pad3;
    float3 HF_RainFogColor;
    float HF_RainGlobalDensity;
    float HF_RainFogHeight;
    float HF_RainHeightFalloff;
    float HF_RainWeightZNear;
    float HF_RainWeightZFar;
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
    float4 SkyClouds : SV_TARGET2;
};

float3 VSPositionFromDepth( float depth, float2 vTexCoord )
{
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord, HF_ProjParams.xy );
}

struct LowCloudDepthFootprint
{
    float closestDepth;
    bool hasSky;
    bool hasGeometry;
};

LowCloudDepthFootprint LoadLowCloudDepthFootprint2x2( float2 texcoord )
{
    uint depthWidth;
    uint depthHeight;
    TX_Depth.GetDimensions( depthWidth, depthHeight );
    int2 depthSize = max( int2( depthWidth, depthHeight ), int2( 1, 1 ) );
    int2 maxPixel = depthSize - int2( 1, 1 );
    int2 upperPixel = int2( floor( texcoord * float2( depthSize ) + 0.5f ) );
    int2 basePixel = upperPixel - int2( 1, 1 );
    int2 pixel00 = clamp( basePixel, int2( 0, 0 ), maxPixel );
    int2 pixel10 = clamp( basePixel + int2( 1, 0 ), int2( 0, 0 ), maxPixel );
    int2 pixel01 = clamp( basePixel + int2( 0, 1 ), int2( 0, 0 ), maxPixel );
    int2 pixel11 = clamp( basePixel + int2( 1, 1 ), int2( 0, 0 ), maxPixel );
    float depth00 = TX_Depth.Load( int3( pixel00, 0 ) ).r;
    float depth10 = TX_Depth.Load( int3( pixel10, 0 ) ).r;
    float depth01 = TX_Depth.Load( int3( pixel01, 0 ) ).r;
    float depth11 = TX_Depth.Load( int3( pixel11, 0 ) ).r;
    const float skyDepthEpsilon = 0.00001f;
    LowCloudDepthFootprint footprint;
    footprint.closestDepth = max( max( depth00, depth10 ), max( depth01, depth11 ) );
    footprint.hasSky = depth00 < skyDepthEpsilon
        || depth10 < skyDepthEpsilon
        || depth01 < skyDepthEpsilon
        || depth11 < skyDepthEpsilon;
    footprint.hasGeometry = depth00 >= skyDepthEpsilon
        || depth10 >= skyDepthEpsilon
        || depth01 >= skyDepthEpsilon
        || depth11 >= skyDepthEpsilon;
    return footprint;
}

PS_OUTPUT PSMain( PS_INPUT Input )
{
    LowCloudDepthFootprint depthFootprint = LoadLowCloudDepthFootprint2x2( Input.vTexcoord );
    float expDepth = depthFootprint.closestDepth;
    float skyPixel = 1.0f - step( 0.00001f, expDepth );
    float3 worldPosition = VSPositionFromDepth( expDepth, Input.vTexcoord );
    worldPosition = mul( float4( worldPosition, 1.0f ), HF_InvView ).xyz;
    float cameraDistance = length( worldPosition - HF_CameraPosition );
    float nightTimeBlend = smoothstep( 0.0f, 1.0f, saturate( -AC_LightPos.y * 4.0f ) )
        * saturate( AC_EnableNightAtmosphere );

    float globalShadow = 0.0f;
    float sunWeight = saturate( AC_SunVisibility ) * smoothstep( 0.04f, 0.42f, AC_LightPos.y );
    float3 sunDir = normalize( lerp( float3( -0.25f, 0.72f, 0.18f ), AC_LightPos, saturate( abs( AC_LightPos.y ) + 0.12f ) ) );
    float3 moonDir = normalize( lerp( float3( 0.22f, 0.64f, -0.28f ), AC_MoonPos, saturate( abs( AC_MoonPos.y ) + 0.12f ) ) );
    float moonDiskWeight = saturate( AC_MoonVisibility ) * saturate( AC_EnableNightAtmosphere ) * smoothstep( 0.02f, 0.34f, AC_MoonPos.y );

    float4 clouds = ComputeWorldLowCloudVolume(
        HF_CameraPosition,
        worldPosition,
        cameraDistance,
        skyPixel,
        HF_FogHeight,
        HF_FogColorMod,
        nightTimeBlend );

    float3 skyWorldPosition = VSPositionFromDepth( 0.0f, Input.vTexcoord );
    skyWorldPosition = mul( float4( skyWorldPosition, 1.0f ), HF_InvView ).xyz;
    float skyCameraDistance = length( skyWorldPosition - HF_CameraPosition );
    float3 skyViewDir = normalize( skyWorldPosition - HF_CameraPosition );
    float skySunAlignment = dot( skyViewDir, sunDir );
    float skyMoonAlignment = dot( skyViewDir, moonDir );
    float skySunCore = smoothstep( 0.99920f, 0.99986f, skySunAlignment ) * sunWeight;
    float skySunHalo = smoothstep( 0.99200f, 0.99860f, skySunAlignment ) * sunWeight;
    float skyMoonCore = smoothstep( 0.99935f, 0.99988f, skyMoonAlignment ) * moonDiskWeight;
    float skyMoonHalo = smoothstep( 0.99500f, 0.99900f, skyMoonAlignment ) * moonDiskWeight;
    float4 skyClouds = clouds;
    if ( depthFootprint.hasSky && depthFootprint.hasGeometry )
    {
        skyClouds = ComputeWorldLowCloudVolume(
            HF_CameraPosition,
            skyWorldPosition,
            skyCameraDistance,
            1.0f,
            HF_FogHeight,
            HF_FogColorMod,
            nightTimeBlend );
    }

    float3 viewDir = normalize( worldPosition - HF_CameraPosition );
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

    float moonDiskMask = saturate( moonCore + moonHalo * 0.24f );
    float moonCoverAtLight = saturate( clouds.a * 1.90f + globalShadow * 0.86f );
    float moonDiskOcclusion =
        saturate( moonDiskMask * moonCoverAtLight )
        * lerp( 0.78f, 0.995f, moonCore );

    float originalCloudAlpha =
        saturate( clouds.a );
    float sunTransmissionMask =
        saturate(
            sunCore * 0.42f
            + sunHalo * 0.18f );
    float mediumDensityGlow =
        saturate(
            1.0f
            - abs( originalCloudAlpha - 0.52f )
                / 0.42f );
    float denseCloudSuppression =
        1.0f
        - smoothstep(
            0.68f,
            0.94f,
            originalCloudAlpha );
    float backlightDensity =
        mediumDensityGlow
        * denseCloudSuppression;
    float sunBacklightMask =
        sunTransmissionMask
        * saturate( sunWeight )
        * max( 0.0f, AC_LowCloudSunLight );
    float3 transmittedSunColor =
        lerp(
            float3( 1.00f, 0.72f, 0.42f ),
            float3( 1.00f, 0.92f, 0.74f ),
            saturate( AC_LightPos.y * 2.5f ) );
    float broadSunMask =
        smoothstep(
            0.82f,
            0.97f,
            sunAlignment )
        * saturate( sunWeight )
        * max( 0.0f, AC_LowCloudSunLight );
    float broadBodyDensity =
        smoothstep(
            0.14f,
            0.46f,
            originalCloudAlpha )
        * ( 1.0f
            - smoothstep(
                0.72f,
                0.95f,
                originalCloudAlpha ) );
    float thinEdgeDensity =
        smoothstep(
            0.05f,
            0.22f,
            originalCloudAlpha )
        * ( 1.0f
            - smoothstep(
                0.30f,
                0.50f,
                originalCloudAlpha ) );
    clouds.rgb +=
        transmittedSunColor
        * sunBacklightMask
        * backlightDensity
        * 0.24f;
    clouds.rgb +=
        transmittedSunColor
        * broadSunMask
        * ( broadBodyDensity * 0.10f
            + thinEdgeDensity * 0.06f );

    float transmittedCloudAlpha = originalCloudAlpha;
    float layerAlpha = saturate( originalCloudAlpha + moonDiskOcclusion * ( 1.0f - originalCloudAlpha ) );
    float skyVeilDistance = SmootherStep01( saturate( ( skyCameraDistance - 3500.0f ) / 52000.0f ) );
    float skyVeilAmount = rainVeil * skyVeilDistance;
    skyClouds.rgb = lerp( skyClouds.rgb, nightRainVeilColor, skyVeilAmount );
    skyClouds.a *= 1.0f - skyVeilAmount * 0.34f;
    float skyTransmittedCloudAlpha = saturate( skyClouds.a );
    float skyMoonDiskMask = saturate( skyMoonCore + skyMoonHalo * 0.24f );
    float skyMoonCoverAtLight = saturate( skyTransmittedCloudAlpha * 1.90f + globalShadow * 0.86f );
    float skyMoonDiskOcclusion =
        saturate( skyMoonDiskMask * skyMoonCoverAtLight )
        * lerp( 0.78f, 0.995f, skyMoonCore );
    float skySunTransmissionMask =
        saturate(
            skySunCore * 0.42f
            + skySunHalo * 0.18f );
    float skyMediumDensityGlow =
        saturate(
            1.0f
            - abs( skyTransmittedCloudAlpha - 0.52f )
                / 0.42f );
    float skyDenseCloudSuppression =
        1.0f
        - smoothstep(
            0.68f,
            0.94f,
            skyTransmittedCloudAlpha );
    float skyBacklightDensity = skyMediumDensityGlow * skyDenseCloudSuppression;
    float skySunBacklightMask =
        skySunTransmissionMask
        * saturate( sunWeight )
        * max( 0.0f, AC_LowCloudSunLight );
    float skyBroadSunMask =
        smoothstep(
            0.82f,
            0.97f,
            skySunAlignment )
        * saturate( sunWeight )
        * max( 0.0f, AC_LowCloudSunLight );
    float skyBroadBodyDensity =
        smoothstep(
            0.14f,
            0.46f,
            skyTransmittedCloudAlpha )
        * ( 1.0f
            - smoothstep(
                0.72f,
                0.95f,
                skyTransmittedCloudAlpha ) );
    float skyThinEdgeDensity =
        smoothstep(
            0.05f,
            0.22f,
            skyTransmittedCloudAlpha )
        * ( 1.0f
            - smoothstep(
                0.30f,
                0.50f,
                skyTransmittedCloudAlpha ) );
    skyClouds.rgb +=
        transmittedSunColor
        * skySunBacklightMask
        * skyBacklightDensity
        * 0.24f;
    skyClouds.rgb +=
        transmittedSunColor
        * skyBroadSunMask
        * ( skyBroadBodyDensity * 0.10f
            + skyThinEdgeDensity * 0.06f );
    float skyLayerAlpha = saturate(
        skyTransmittedCloudAlpha
        + skyMoonDiskOcclusion * ( 1.0f - skyTransmittedCloudAlpha ) );
    PS_OUTPUT output;
    output.Clouds = float4( clouds.rgb * transmittedCloudAlpha, layerAlpha );
    output.Depth = expDepth;
    output.SkyClouds = depthFootprint.hasSky
        ? float4( skyClouds.rgb * skyTransmittedCloudAlpha, skyLayerAlpha )
        : float4( 0.0f, 0.0f, 0.0f, -1.0f );
    return output;
}
