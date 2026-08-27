//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

#include <AtmosphericScattering.h>
#include "DepthReconstruction.h"

cbuffer PFXBuffer : register( b0 )
{
	float4 HF_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
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

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Texture0 : register( t0 );
Texture2D	TX_Depth : register( t1 );
Texture2D	TX_FogBlueNoise : register( t2 );

float3 VSPositionFromDepth(float depth, float2 vTexCoord)
{
	return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord, HF_ProjParams.xy );
}

float ComputeVolumetricFogCandidate(
float3 cameraToWorldPos,
float3 posOriginal,
float heightFalloff,
float globalDensity,
float weightZNear,
float weightZFar)
{
float cVolFogHeightDensityAtViewer = exp( -heightFalloff );
float lenOrig = length(posOriginal - HF_CameraPosition);
float len = length(cameraToWorldPos);
float fogInt = len * cVolFogHeightDensityAtViewer;
const float cSlopeThreshold = 0.01;
float w = saturate((lenOrig - weightZNear) / max(weightZFar - weightZNear, 1.0f));
if(abs( cameraToWorldPos.y ) > cSlopeThreshold )
{
float t = heightFalloff * cameraToWorldPos.y * w;
fogInt *= (abs( t ) > 0.0001 ? ( ( 1.0 - exp( -t ) ) / t ) : 1.0);
}
return exp( -globalDensity * w * fogInt );
}

float GetFogBlueNoise( float2 pixelPosition )
{
    uint frame = (uint)max( AC_Time * 60.0f, 0.0f );
    uint2 pixel = uint2( max( pixelPosition, 0.0f ) );
    uint2 noiseCoord = (pixel + uint2( frame * 17u, frame * 29u )) & uint2( 511u, 511u );
    float4 noiseSample = TX_FogBlueNoise.Load( int3( noiseCoord, 0 ) );
    uint channel = frame & 3u;
    return noiseSample[channel] * 2.0f - 1.0f;
}

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float3 vEyeRay			: TEXCOORD1;
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float expDepth = TX_Depth.Sample(SS_Linear, Input.vTexcoord).r;
	float skyPixel = 1.0f - step(0.00001f, expDepth);
	
	float3 position = VSPositionFromDepth(expDepth, Input.vTexcoord);
	
	
	position = mul(float4(position, 1), HF_InvView).xyz;
	float3 posOriginal = position;
	
	position -= HF_CameraPosition;
	position.y -= HF_FogHeight;
	float worldFog = 1.0f - ComputeVolumetricFogCandidate(
	position,
	posOriginal,
	HF_HeightFalloff,
	HF_GlobalDensity,
	HF_WeightZNear,
	HF_WeightZFar);
	float activeWeatherFog = saturate(AC_RainFXWeight);
	float nightTimeBlend = smoothstep(0.0f, 1.0f, saturate(-AC_LightPos.y * 4.0f))
	* saturate(AC_EnableNightAtmosphere)
	* saturate(AC_NightFogEnabled);
	float worldFogActivation = max(HF_FogOverride, nightTimeBlend * (1.0f - activeWeatherFog));
	worldFog *= worldFogActivation;
	// Explicit Gothic world-fog zones conceal distant geometry identically at
	// every time of day. Rain remains an independent veil composed later.
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
	float3 worldFogColor = ApplyAtmosphericScatteringGround(
		worldFogColorPosition,
		HF_FogColorMod,
		true,
		false);
	// Match composition: the fully occluded regional-fog field is a
	// uniform zone color, so distant terrain cannot survive as a blue silhouette.
	worldFogColor = lerp(
		worldFogColor,
		HF_FogColorMod,
		worldFogFarOcclusion);
	float nightFogBrightness = lerp(1.0f, max(0.0f, AC_NightFogBrightness), saturate(AC_EnableNightAtmosphere));
	float3 nightFogColor = float3(0.12f, 0.18f, 0.27f) * nightFogBrightness;
	worldFogColor = lerp(worldFogColor, nightFogColor, nightTimeBlend);
	float dayDarknessFactor = max(1.0f, 2.0f - max(0.0f, AC_LightPos.y));
	float darknessFactor = lerp(dayDarknessFactor, 2.0f, nightTimeBlend);
	float maxFogOpacity = lerp(1.0f, 0.85f, nightTimeBlend);
	float worldFogOpacity = saturate(worldFog) * maxFogOpacity;
	worldFogOpacity = max(worldFogOpacity, worldFogFarOcclusion);
	float3 rainPosition = posOriginal - HF_CameraPosition;
	rainPosition.y -= HF_RainFogHeight;
	float rainFog = 1.0f - ComputeVolumetricFogCandidate(
	rainPosition,
	posOriginal,
	HF_RainHeightFalloff,
	HF_RainGlobalDensity,
	HF_RainWeightZNear,
	HF_RainWeightZFar);
	float3 finalWorldFogColor = saturate(worldFogColor / darknessFactor);
	float3 nightRainVeilColor = nightFogColor / 2.0f;
	float3 rainVeilColor = lerp(HF_RainFogColor, nightRainVeilColor, nightTimeBlend);
	float reducedRainFogOpacity = saturate(rainFog) * 0.75f;
	float rainVeilBase = lerp(0.040f, 0.17f, nightTimeBlend);
	float rainFogOpacity = max(reducedRainFogOpacity, rainVeilBase) * activeWeatherFog;
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
		nightTimeBlend);
	worldFogReferenceOpacity = saturate(worldFogReferenceOpacity)
		* worldFogReferenceMaxOpacity
		* saturate(HF_FogOverride);
	rainFogReferenceOpacity = max(
		saturate(rainFogReferenceOpacity) * 0.75f,
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
	float globalRainWinnerBlend = rainFogPresent * (
		(1.0f - worldFogEventPresent)
		+ worldFogEventPresent * transitionRainWinnerBlend);
	float finalFogOpacity = lerp(
		worldFogOpacity,
		rainFogOpacity,
		globalRainWinnerBlend);
	float3 finalFogColor = lerp(
		finalWorldFogColor,
		rainVeilColor,
		globalRainWinnerBlend);

	// Keep the fallback fog consistent with the normal composition path. The
	// neutral blue-noise offset is limited to dark, partially transparent night
	// gradients so it cannot become visible grain in clear or opaque fog.
	float fogGradientMask = saturate(finalFogOpacity * (1.0f - finalFogOpacity) * 4.0f);
	float fogDarknessMask = 1.0f - smoothstep(
		0.32f,
		0.78f,
		dot(finalFogColor, float3(0.2126f, 0.7152f, 0.0722f)));
	float fogDitherMask = fogGradientMask * fogDarknessMask * saturate(nightTimeBlend);
	finalFogOpacity = saturate(finalFogOpacity + GetFogBlueNoise(Input.vPosition.xy)
		* (1.0f / 255.0f) * fogDitherMask);
	return float4(finalFogColor, finalFogOpacity);
}
