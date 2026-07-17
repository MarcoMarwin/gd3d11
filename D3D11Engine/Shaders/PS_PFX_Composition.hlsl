//--------------------------------------------------------------------------------------
// PostFX Composition Uber Shader
// Merges atmospheric and screen-space lighting effects into a single full-screen pass.
// Permutation macros select HeightFog, GodRays, contact shadows and SSGI.
//--------------------------------------------------------------------------------------

#if COMPOSE_HEIGHTFOG
#include <AtmosphericScattering.h>
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
    float HF_pad2;

    float2 HF_ProjAB;
    float2 HF_Pad3;
};

#endif

#if COMPOSE_HEIGHTFOG || COMPOSE_CONTACT_SHADOWS
cbuffer CompositionControl : register( b2 )
{
    float CC_HeightFogEnabled;
    float CC_ContactShadowScale;
    float2 CC_Pad;
};
#endif

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );

Texture2D TX_Backbuffer : register( t0 );

#if COMPOSE_GODRAYS
Texture2D TX_GodRays : register( t1 );
#endif

#if COMPOSE_HEIGHTFOG
Texture2D TX_Depth : register( t2 );
#endif

#if COMPOSE_CONTACT_SHADOWS || COMPOSE_SSGI
Texture2D TX_ScreenSpaceLighting : register( t5 );
#endif

//--------------------------------------------------------------------------------------
// HeightFog helpers (inlined from PS_PFX_Heightfog.hlsl)
//--------------------------------------------------------------------------------------
#if COMPOSE_HEIGHTFOG
float3 VSPositionFromDepth( float depth, float2 vTexCoord )
{
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord, HF_ProjParams.xy );
}

float ComputeVolumetricFog( float3 cameraToWorldPos, float3 posOriginal )
{
    float cVolFogHeightDensityAtViewer = exp( -HF_HeightFalloff );

    float lenOrig = length( posOriginal - HF_CameraPosition );
    float len = length( cameraToWorldPos );
    float fogInt = len * cVolFogHeightDensityAtViewer;
    const float cSlopeThreshold = 0.01;

    float w = saturate( ( lenOrig - HF_WeightZNear ) / ( HF_WeightZFar - HF_WeightZNear ) );

    if ( abs( cameraToWorldPos.y ) > cSlopeThreshold )
    {
        float t = HF_HeightFalloff * cameraToWorldPos.y * w;
        fogInt *= ( abs( t ) > 0.0001 ? ( ( 1.0 - exp( -t ) ) / t ) : 1.0 );
    }

    return exp( -HF_GlobalDensity * w * fogInt );
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

    float fog = 1.0f - ComputeVolumetricFog( position, posOriginal );
    float fogDistance = length(posOriginal - HF_CameraPosition);
    float activeWeatherFog = saturate(AC_RainFXWeight);
	float nightTimeBlend = smoothstep(0.0f, 1.0f, saturate(-AC_LightPos.y * 4.0f))
		* saturate(AC_EnableNightAtmosphere);
    float stableFadeEnd = max(HF_WeightZFar * lerp(1.12f, 1.32f, nightTimeBlend * activeWeatherFog), 1000.0f);
    float stableFadeStart = max(HF_WeightZNear * 0.55f, stableFadeEnd * lerp(0.34f, 0.48f, nightTimeBlend * activeWeatherFog));
    float stableWorldFade = SmootherStep01(saturate((fogDistance - stableFadeStart) / max(1.0f, stableFadeEnd - stableFadeStart)));
    float weatherFog = max(fog, stableWorldFade) * activeWeatherFog;
    float dryNightFog = fog * nightTimeBlend * (1.0f - activeWeatherFog);
    fog = max(weatherFog, dryNightFog);

    float3 color = ApplyAtmosphericScatteringGround( position, HF_FogColorMod, true, false );
	float nightFogBrightness = lerp(1.0f, max(0.0f, AC_NightFogBrightness), saturate(AC_EnableNightAtmosphere));
	float3 nightFogColor = float3(0.12f, 0.18f, 0.27f) * nightFogBrightness;
	color = lerp(color, nightFogColor, nightTimeBlend);
	float dayDarknessFactor = max(1.0f, 2.0f - max(0.0f, AC_LightPos.y));
	float darknessFactor = lerp(dayDarknessFactor, 2.5f, nightTimeBlend);
	float maxFogOpacity = lerp(1.0f, 0.85f, nightTimeBlend);
    float skyRainFogOpacity = lerp(0.90f, 0.85f, nightTimeBlend);
    maxFogOpacity = lerp(maxFogOpacity, min(maxFogOpacity, skyRainFogOpacity), skyPixel * activeWeatherFog);

	return float4(saturate(color / darknessFactor), fog * maxFogOpacity);
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


//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float4 color = TX_Backbuffer.Sample( SS_Linear, Input.vTexcoord );

    // Composition order: HeightFog and screen-space lighting, then GodRays.

#if COMPOSE_CONTACT_SHADOWS || COMPOSE_SSGI
    float4 screenSpaceLighting = TX_ScreenSpaceLighting.SampleLevel( SS_Linear, Input.vTexcoord, 0 );
#endif

#if COMPOSE_CONTACT_SHADOWS
    color.rgb *= 1.0f - saturate(
        screenSpaceLighting.a * CC_ContactShadowScale );
#endif

#if COMPOSE_SSGI
    color.rgb += max( screenSpaceLighting.rgb, 0.0f );
#endif

#if COMPOSE_HEIGHTFOG
    [branch] if ( CC_HeightFogEnabled > 0.5f )
    {
        float4 fog = ComputeHeightFog( Input.vTexcoord );
        color.rgb = lerp( color.rgb, fog.rgb, fog.a );
        float nightTimeBlend = smoothstep(0.0f, 1.0f, saturate(-AC_LightPos.y * 4.0f));
        float nightAtmosphereBlend = nightTimeBlend * saturate(AC_EnableNightAtmosphere);
        float activeWeatherFog = saturate(AC_RainFXWeight);
        float nightFogBrightness = lerp(1.0f, max(0.0f, AC_NightFogBrightness), saturate(AC_EnableNightAtmosphere));
        float3 nightRainVeilColor = float3(0.12f, 0.18f, 0.27f) * nightFogBrightness / 2.5f;
        float3 rainVeilColor = lerp(fog.rgb, nightRainVeilColor, nightAtmosphereBlend);
        float rainVeilBase = activeWeatherFog * lerp(0.050f, 0.22f, nightAtmosphereBlend);
        float rainVeilSpatial = lerp(0.45f, 1.0f, SmootherStep01(saturate(fog.a * 1.35f)));
        float rainVeil = rainVeilBase * rainVeilSpatial;
        color.rgb = lerp(color.rgb, rainVeilColor, rainVeil);
    }
#endif

#if COMPOSE_GODRAYS
    float3 godrays = TX_GodRays.Sample( SS_Linear, Input.vTexcoord ).rgb;
    color.rgb += godrays;
#endif


    return color;
}
