//--------------------------------------------------------------------------------------
// PostFX Low Clouds Composite
// Upsamples a premultiplied low-cloud layer and blends it over the full-resolution scene.
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
Texture2D TX_Backbuffer : register( t0 );
Texture2D TX_LowClouds : register( t1 );
Texture2D TX_LowCloudDepth : register( t2 );
Texture2D TX_FullDepth : register( t3 );
Texture2D TX_SkyLowClouds : register( t4 );

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float LoadLowCloudDepth( int2 cloudPixel, int2 cloudSize )
{
    cloudPixel = clamp(
        cloudPixel, int2( 0, 0 ), cloudSize - int2( 1, 1 ) );
    return TX_LowCloudDepth.Load( int3( cloudPixel, 0 ) ).r;
}

float4 SampleStableSkyLowClouds( float2 texcoord )
{
    uint cloudWidth;
    uint cloudHeight;
    TX_SkyLowClouds.GetDimensions( cloudWidth, cloudHeight );
    int2 cloudSize = max( int2( cloudWidth, cloudHeight ), int2( 1, 1 ) );
    float2 cloudPosition = texcoord * float2( cloudSize ) - 0.5f;
    int2 centerCloudPixel = int2( floor( cloudPosition + 0.5f ) );
    float4 filteredClouds = 0.0f;
    float4 bestClouds = 0.0f;
    float totalWeight = 0.0f;
    [unroll]
    for ( int y = -1; y <= 1; ++y )
    {
        [unroll]
        for ( int x = -1; x <= 1; ++x )
        {
            int2 cloudPixel = clamp(
                centerCloudPixel + int2( x, y ),
                int2( 0, 0 ),
                cloudSize - int2( 1, 1 ) );
            float4 sampleClouds =
                TX_SkyLowClouds.Load( int3( cloudPixel, 0 ) );
            if ( sampleClouds.a < 0.0f )
            {
                continue;
            }
            float2 spatialDelta = float2( cloudPixel ) - cloudPosition;
            float spatialWeight = exp2(
                -dot( spatialDelta, spatialDelta ) * 0.55f );
            float alphaWeight = lerp(
                0.35f, 0.85f, saturate( sampleClouds.a * 2.0f ) );
            float weight = spatialWeight * alphaWeight;
            filteredClouds += sampleClouds * weight;
            totalWeight += weight;
            if ( sampleClouds.a > bestClouds.a )
            {
                bestClouds = sampleClouds;
            }
        }
    }
    if ( totalWeight > 0.00001f )
    {
        float4 averagedClouds = filteredClouds / totalWeight;
        return lerp( averagedClouds, bestClouds, 0.20f );
    }
    return float4( 0.0f, 0.0f, 0.0f, 0.0f );
}

float GetLowCloudDepthWeight( float targetDepth, float sourceDepth )
{
    const float skyDepthEpsilon = 0.00001f;
    bool targetIsSky = targetDepth < skyDepthEpsilon;
    bool sourceIsSky = sourceDepth < skyDepthEpsilon;
    if ( targetIsSky != sourceIsSky )
    {
        return 0.0f;
    }
    if ( targetIsSky )
    {
        return 1.0f;
    }

    float relativeDepthDelta = abs( targetDepth - sourceDepth )
        / max( max( targetDepth, sourceDepth ), skyDepthEpsilon );
    if ( relativeDepthDelta >= 0.18f )
    {
        return 0.0f;
    }
    return exp2( -relativeDepthDelta * 32.0f );
}

float4 ComputeRefinedLowClouds( float2 texcoord, float depth )
{
    float3 viewPosition = ReconstructVSPositionFromDepthReverseZInfinite(
        depth, texcoord, HF_ProjParams.xy );
    float3 worldPosition = mul( float4( viewPosition, 1.0f ), HF_InvView ).xyz;
    float cameraDistance = length( worldPosition - HF_CameraPosition );
    float nightTimeBlend = smoothstep( 0.0f, 1.0f, saturate( -AC_LightPos.y * 4.0f ) )
        * saturate( AC_EnableNightAtmosphere );
    float sunWeight = saturate( AC_SunVisibility )
        * smoothstep( 0.04f, 0.42f, AC_LightPos.y );
    float4 clouds = ComputeWorldLowCloudVolumeWithSteps(
        HF_CameraPosition, worldPosition, cameraDistance, 0.0f,
        HF_FogHeight, HF_FogColorMod, nightTimeBlend, 4 );
    float nightFogBrightness = lerp( 1.0f, max( 0.0f, AC_NightFogBrightness ),
        saturate( AC_EnableNightAtmosphere ) );
    float3 nightRainVeilColor = float3( 0.12f, 0.18f, 0.27f )
        * nightFogBrightness / 2.5f;
    float rainVeil = saturate( AC_RainFXWeight )
        * lerp( 0.045f, 0.30f, nightTimeBlend );
    float veilDistance = SmootherStep01( saturate(
        ( cameraDistance - 3500.0f ) / 52000.0f ) );
    float veilAmount = rainVeil * 0.45f * veilDistance;
    clouds.rgb = lerp( clouds.rgb, nightRainVeilColor, veilAmount );
    clouds.a *= 1.0f - veilAmount * 0.34f;
    float originalCloudAlpha = saturate( clouds.a );
    float3 viewDir = normalize( worldPosition - HF_CameraPosition );
    float3 transmittedSunColor = lerp(
        float3( 1.00f, 0.72f, 0.42f ),
        float3( 1.00f, 0.92f, 0.74f ),
        saturate( AC_LightPos.y * 2.5f ) );
    float broadSunMask = smoothstep( 0.82f, 0.97f,
        dot( viewDir, normalize( lerp(
            float3( -0.25f, 0.72f, 0.18f ), AC_LightPos,
            saturate( abs( AC_LightPos.y ) + 0.12f ) ) ) ) )
        * sunWeight * max( 0.0f, AC_LowCloudSunLight );
    float broadBodyDensity = smoothstep( 0.14f, 0.46f, originalCloudAlpha )
        * ( 1.0f - smoothstep( 0.72f, 0.95f, originalCloudAlpha ) );
    float thinEdgeDensity = smoothstep( 0.05f, 0.22f, originalCloudAlpha )
        * ( 1.0f - smoothstep( 0.30f, 0.50f, originalCloudAlpha ) );
    clouds.rgb += transmittedSunColor * broadSunMask
        * ( broadBodyDensity * 0.10f + thinEdgeDensity * 0.06f );
    return float4( clouds.rgb * originalCloudAlpha, originalCloudAlpha );
}

float4 SampleDepthAwareLowClouds(
    float2 texcoord, float4 pixelPosition )
{
    uint cloudWidth;
    uint cloudHeight;
    uint depthWidth;
    uint depthHeight;
    TX_LowClouds.GetDimensions( cloudWidth, cloudHeight );
    TX_FullDepth.GetDimensions( depthWidth, depthHeight );
    int2 cloudSize = max( int2( cloudWidth, cloudHeight ), int2( 1, 1 ) );
    int2 depthSize = max( int2( depthWidth, depthHeight ), int2( 1, 1 ) );
    int2 targetPixel = clamp( int2( pixelPosition.xy ),
        int2( 0, 0 ), depthSize - int2( 1, 1 ) );
    float targetDepth = TX_FullDepth.Load( int3( targetPixel, 0 ) ).r;
    const float skyDepthEpsilon = 0.00001f;
    if ( targetDepth < skyDepthEpsilon )
    {
        return SampleStableSkyLowClouds( texcoord );
    }
    float2 cloudPosition = texcoord * float2( cloudSize ) - 0.5f;
    int2 baseCloudPixel = int2( floor( cloudPosition ) );
    float2 cloudFraction = frac( cloudPosition );
    float4 filteredClouds = 0.0f;
    float totalWeight = 0.0f;
    [unroll]
    for ( int y = 0; y < 2; ++y )
    {
        [unroll]
        for ( int x = 0; x < 2; ++x )
        {
            int2 cloudPixel = clamp( baseCloudPixel + int2( x, y ),
                int2( 0, 0 ), cloudSize - int2( 1, 1 ) );
            float spatialWeight =
                (x == 0 ? 1.0f - cloudFraction.x : cloudFraction.x)
                * (y == 0 ? 1.0f - cloudFraction.y : cloudFraction.y);
            float sourceDepth = LoadLowCloudDepth( cloudPixel, cloudSize );
            float weight = spatialWeight
                * GetLowCloudDepthWeight( targetDepth, sourceDepth );
            filteredClouds += TX_LowClouds.Load( int3( cloudPixel, 0 ) ) * weight;
            totalWeight += weight;
        }
    }
    const float refinementStartWeight = 0.45f;
    const float confidentFootprintWeight = 0.60f;
    if ( totalWeight >= confidentFootprintWeight )
    {
        return filteredClouds / totalWeight;
    }
    float4 refinedClouds = ComputeRefinedLowClouds( texcoord, targetDepth );
    if ( totalWeight <= refinementStartWeight )
    {
        return refinedClouds;
    }
    float4 localClouds = filteredClouds / confidentFootprintWeight;
    float localTrust = smoothstep(
        refinementStartWeight, confidentFootprintWeight, totalWeight );
    return lerp( refinedClouds, localClouds, localTrust );
}

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float4 scene = TX_Backbuffer.Sample( SS_Linear, Input.vTexcoord );
    float4 clouds = SampleDepthAwareLowClouds(
        Input.vTexcoord, Input.vPosition );

    float rainWeight = saturate(AC_RainFXWeight);
    float nightTimeBlend = smoothstep(0.0f, 1.0f, saturate(-AC_LightPos.y * 4.0f))
        * saturate(AC_EnableNightAtmosphere);
    float rainCloudVisibility = 1.0f - smoothstep(0.18f, 0.88f, rainWeight);
    float rainVeil = rainWeight * lerp(0.050f, 0.22f, nightTimeBlend);
    float dryNightVeil = (1.0f - rainWeight) * nightTimeBlend * 0.12f;
    float totalVeil = saturate(rainVeil + dryNightVeil);
    float cloudAlpha = saturate(clouds.a) * rainCloudVisibility;
    clouds.rgb *= rainCloudVisibility;

    if (cloudAlpha > 0.001f && totalVeil > 0.0001f)
    {
        float3 cloudColor = clouds.rgb / max(cloudAlpha, 0.001f);
        cloudColor = lerp(cloudColor, scene.rgb, totalVeil * lerp(0.65f, 1.0f, nightTimeBlend));
        cloudAlpha *= 1.0f - totalVeil * lerp(0.08f, 0.22f, nightTimeBlend);
        clouds.rgb = cloudColor * cloudAlpha;
        clouds.a = cloudAlpha;
    }

    float sunDistance =
        length( Input.vTexcoord - AC_LightScreenPos.xy );
    float moonDistance =
        length( Input.vTexcoord - AC_MoonScreenPos.xy );
    float sunVisibility =
        saturate( AC_LightScreenPos.z )
        * saturate( AC_SunVisibility );
    float moonVisibility =
        saturate( AC_MoonScreenPos.z )
        * saturate( AC_MoonVisibility );
    float sunPreservationMask =
        ( 1.0f - smoothstep( 0.018f, 0.060f, sunDistance ) )
        * sunVisibility;
    float moonPreservationMask =
        ( 1.0f - smoothstep( 0.018f, 0.060f, moonDistance ) )
        * moonVisibility;
    float celestialPreservationMask =
        max( sunPreservationMask, moonPreservationMask );
    float effectiveCloudAlpha =
        min(
            cloudAlpha,
            lerp(
                1.0f,
                0.88f,
                celestialPreservationMask ) );
    float cloudPremultipliedScale =
        cloudAlpha > 0.00001f
            ? effectiveCloudAlpha / cloudAlpha
            : 0.0f;
    scene.rgb =
        scene.rgb * ( 1.0f - effectiveCloudAlpha )
        + clouds.rgb * cloudPremultipliedScale;
    return scene;
}
