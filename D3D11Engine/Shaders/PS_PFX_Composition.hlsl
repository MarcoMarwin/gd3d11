//--------------------------------------------------------------------------------------
// PostFX Composition Uber Shader
// Merges atmospheric and GodRay effects into a single full-screen pass.
//--------------------------------------------------------------------------------------

#if COMPOSE_HEIGHTFOG
#include <AtmosphericScattering.h>
#endif
#if COMPOSE_HEIGHTFOG
#include "DepthReconstruction.h"
#endif

//--------------------------------------------------------------------------------------
// Constant Buffers
//--------------------------------------------------------------------------------------
#if COMPOSE_HEIGHTFOG
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
    float HF_NightFogRainFade;
    float HF_Pad3;
    float3 HF_RainFogColor;
    float HF_RainGlobalDensity;
    float HF_RainFogHeight;
    float HF_RainHeightFalloff;
    float HF_RainWeightZNear;
    float HF_RainWeightZFar;
};

#endif

cbuffer CompositionControl : register( b2 )
{
    float CC_HeightFogEnabled;
    float CC_GodRaysEnabled;
    float2 CC_Pad;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );

Texture2D TX_Backbuffer : register( t0 );

Texture2D TX_GodRays : register( t1 );

#if COMPOSE_HEIGHTFOG
Texture2D TX_Depth : register( t2 );
#endif

//--------------------------------------------------------------------------------------
// HeightFog helpers (inlined from PS_PFX_Heightfog.hlsl)
//--------------------------------------------------------------------------------------
#if COMPOSE_HEIGHTFOG
float3 VSPositionFromDepth( float depth, float2 vTexCoord )
{
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord, HF_ProjParams.xy );
}

float ComputeVolumetricFogCandidate(
float3 cameraToWorldPos,
float3 posOriginal,
float heightFalloff,
float globalDensity,
float weightZNear,
float weightZFar )
{
float cVolFogHeightDensityAtViewer = exp( -heightFalloff );
float lenOrig = length( posOriginal - HF_CameraPosition );
float len = length( cameraToWorldPos );
float fogInt = len * cVolFogHeightDensityAtViewer;
const float cSlopeThreshold = 0.01;
float w = saturate( ( lenOrig - weightZNear ) / max( weightZFar - weightZNear, 1.0f ) );
if ( abs( cameraToWorldPos.y ) > cSlopeThreshold )
{
float t = heightFalloff * cameraToWorldPos.y * w;
fogInt *= ( abs( t ) > 0.0001 ? ( ( 1.0 - exp( -t ) ) / t ) : 1.0 );
}
return exp( -globalDensity * w * fogInt );
}

float4 ComputeHeightFog( float2 texcoord )
{
    float expDepth = TX_Depth.Sample( SS_Linear, texcoord ).r;
    float skyPixel = 1.0f - step(0.00001f, expDepth);
    float3 position = VSPositionFromDepth( expDepth, texcoord );
    position = mul( float4( position, 1 ), HF_InvView ).xyz;
    float3 posOriginal = position;
position -= HF_CameraPosition;
position.y -= HF_FogHeight;
float worldFog = 1.0f - ComputeVolumetricFogCandidate(
position,
posOriginal,
HF_HeightFalloff,
HF_GlobalDensity,
HF_WeightZNear,
HF_WeightZFar );
float activeWeatherFog = saturate(AC_RainFXWeight);
float nightTimeBlend = smoothstep(0.0f, 1.0f, saturate(-AC_LightPos.y * 4.0f))
* saturate(AC_EnableNightAtmosphere)
* saturate(AC_NightFogEnabled);
float nightFogRainFade = saturate(HF_NightFogRainFade);
float worldFogActivation = max(HF_FogOverride, nightTimeBlend * (1.0f - nightFogRainFade));
worldFog *= worldFogActivation;
// Explicit Gothic world-fog zones conceal distant geometry identically at all
// times of day. Rain remains an independent veil composed later.
float worldFogGeometryWeight = saturate(HF_FogOverride) * (1.0f - skyPixel);
float worldFogGeometryDistance = length(posOriginal - HF_CameraPosition);
float worldFogOcclusionStart = lerp(HF_WeightZNear, HF_WeightZFar, 0.45f);
float worldFogOcclusionEnd = lerp(HF_WeightZNear, HF_WeightZFar, 0.82f);
float worldFogFarOcclusion = smoothstep(
    worldFogOcclusionStart,
    max(worldFogOcclusionEnd, worldFogOcclusionStart + 1.0f),
    worldFogGeometryDistance) * worldFogGeometryWeight;
worldFog = max(worldFog, worldFogFarOcclusion);
float3 worldFogColorPosition = lerp(
    position, posOriginal, worldFogGeometryWeight);
float3 color = ApplyAtmosphericScatteringGround(
    worldFogColorPosition,
    HF_FogColorMod,
    true,
    false);
// Once regional fog has reached its opaque far field, positional atmospheric
// tint must no longer encode the hidden terrain silhouette.
color = lerp(color, HF_FogColorMod, worldFogFarOcclusion);
float nightFogBrightness = lerp(1.0f, max(0.0f, AC_NightFogBrightness), saturate(AC_EnableNightAtmosphere));
float3 nightFogColor = float3(0.12f, 0.18f, 0.27f) * nightFogBrightness;
color = lerp(color, nightFogColor, nightTimeBlend);
float dayDarknessFactor = max(1.0f, 2.0f - max(0.0f, AC_LightPos.y));
float darknessFactor = lerp(dayDarknessFactor, 2.5f, nightTimeBlend);
float maxFogOpacity = lerp(1.0f, 0.85f, nightTimeBlend);
float worldFogOpacity = saturate(worldFog) * maxFogOpacity;
worldFogOpacity = max(worldFogOpacity, worldFogFarOcclusion);
return float4(saturate(color / darknessFactor), saturate(worldFogOpacity));
}
#endif

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
    float2 vTexcoord  : TEXCOORD0;
    float3 vEyeRay    : TEXCOORD1;
    float4 vPosition  : SV_POSITION;
};

struct PS_OUTPUT
{
    float4 color : SV_TARGET0;
    float fogCompositionMask : SV_TARGET1;
    float fogReactiveMask : SV_TARGET2;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
PS_OUTPUT PSMain( PS_INPUT Input )
{
    float4 color = TX_Backbuffer.Sample( SS_Linear, Input.vTexcoord );
    float fogCompositionMask = 0.0f;
    float fogReactiveMask = 0.0f;

    // Composition order: HeightFog, then GodRays.

#if COMPOSE_HEIGHTFOG
    [branch] if ( CC_HeightFogEnabled > 0.5f )
    {
        float4 fog = ComputeHeightFog( Input.vTexcoord );
float expDepth = TX_Depth.Sample( SS_Linear, Input.vTexcoord ).r;
float3 rainPosition = VSPositionFromDepth( expDepth, Input.vTexcoord );
rainPosition = mul( float4( rainPosition, 1 ), HF_InvView ).xyz;
float3 rainPosOriginal = rainPosition;
rainPosition -= HF_CameraPosition;
rainPosition.y -= HF_RainFogHeight;
float nightTimeBlend = smoothstep(0.0f, 1.0f, saturate(-AC_LightPos.y * 4.0f))
    * saturate(AC_NightFogEnabled);
float nightAtmosphereBlend = nightTimeBlend * saturate(AC_EnableNightAtmosphere);
float activeWeatherFog = saturate(AC_RainFXWeight);
float nightFogRainFade = saturate(HF_NightFogRainFade);
float rainFog = 1.0f - ComputeVolumetricFogCandidate(
rainPosition,
rainPosOriginal,
HF_RainHeightFalloff,
HF_RainGlobalDensity,
HF_RainWeightZNear,
HF_RainWeightZFar );
float nightFogBrightness = lerp(1.0f, max(0.0f, AC_NightFogBrightness), saturate(AC_EnableNightAtmosphere));
float3 nightRainVeilColor = float3(0.12f, 0.18f, 0.27f) * nightFogBrightness / 2.5f;
float3 rainVeilColor = lerp(HF_RainFogColor, nightRainVeilColor, nightAtmosphereBlend);
float rainFogOpacity = saturate(rainFog) * 0.60f;
float rainVeilBase = lerp(0.032f, 0.135f, nightAtmosphereBlend);
float rainVeil = max(rainFogOpacity, rainVeilBase) * activeWeatherFog;
        float worldFogEventPresent = step(0.0001f, HF_FogOverride);
        float rainFogPresent = step(0.0001f, activeWeatherFog);
        float worldFogReferenceDistance = max(lerp(
            HF_WeightZNear,
            HF_WeightZFar,
            0.75f), 0.0f);
        float rainFogReferenceDistance = max(lerp(
            HF_RainWeightZNear,
            HF_RainWeightZFar,
            0.75f), 0.0f);
        float worldFogReferenceOpacity = 1.0f - exp(
            -max(HF_GlobalDensity, 0.0f)
            * 0.75f
            * worldFogReferenceDistance
            * exp(-HF_HeightFalloff));
        float rainFogReferenceOpacity = 1.0f - exp(
            -max(HF_RainGlobalDensity, 0.0f)
            * 0.75f
            * rainFogReferenceDistance
            * exp(-HF_RainHeightFalloff));
        float worldFogReferenceMaxOpacity = lerp(
            1.0f,
            0.85f,
            nightAtmosphereBlend);
        worldFogReferenceOpacity = saturate(worldFogReferenceOpacity)
            * worldFogReferenceMaxOpacity
            * saturate(HF_FogOverride);
        rainFogReferenceOpacity = max(
            saturate(rainFogReferenceOpacity) * 0.60f,
            rainVeilBase)
            * activeWeatherFog;
        float strongestReferenceOpacity = max(
            max(worldFogReferenceOpacity, rainFogReferenceOpacity),
            0.0001f);
        float globalFogDominance = (
            rainFogReferenceOpacity - worldFogReferenceOpacity)
            / strongestReferenceOpacity;
        float transitionRainWinnerBlend = smoothstep(
            -0.08f,
            0.08f,
            globalFogDominance);
        float rainVisibilityBlend = smoothstep(0.0f, 0.18f, activeWeatherFog);
        float rainDrivenNightFogBlend = rainFogPresent * nightFogRainFade;
        float rainDrivenRainFogBlend = lerp(
            rainVisibilityBlend,
            rainDrivenNightFogBlend,
            nightAtmosphereBlend);
        float globalRainWinnerBlend = rainDrivenRainFogBlend * (
            (1.0f - worldFogEventPresent)
            + worldFogEventPresent * transitionRainWinnerBlend);
        float finalFogWeight = lerp(
            fog.a,
            rainVeil,
            globalRainWinnerBlend);
        float3 finalFogColor = lerp(
            fog.rgb,
            rainVeilColor,
            globalRainWinnerBlend);

        color.rgb = lerp(color.rgb, finalFogColor, finalFogWeight);

        fogCompositionMask = saturate(finalFogWeight);
        fogReactiveMask = fogCompositionMask * 0.20f;
    }
#endif

#if COMPOSE_GODRAYS
    [branch] if ( CC_GodRaysEnabled > 0.5f )
    {
        float3 godrays = TX_GodRays.Sample( SS_Linear, Input.vTexcoord ).rgb;
        color.rgb += godrays;
    }
#endif


    PS_OUTPUT output;
    output.color = color;
    output.fogCompositionMask = fogCompositionMask;
    output.fogReactiveMask = fogReactiveMask;
    return output;
}
