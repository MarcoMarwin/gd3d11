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
	float HF_pad2;

	float2 HF_ProjAB;
	float2 HF_Pad3;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Texture0 : register( t0 );
Texture2D	TX_Depth : register( t1 );

float3 VSPositionFromDepth(float depth, float2 vTexCoord)
{
	return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord, HF_ProjParams.xy );
}

float ComputeVolumetricFog(float3 cameraToWorldPos, float3 posOriginal)
{	
	float cVolFogHeightDensityAtViewer = exp( -HF_HeightFalloff );
	
	float lenOrig = length(posOriginal - HF_CameraPosition);
	float len = length(cameraToWorldPos);
	float fogInt = len * cVolFogHeightDensityAtViewer;
	const float	cSlopeThreshold = 0.01;
	
	float w = saturate((lenOrig-HF_WeightZNear)/(HF_WeightZFar-HF_WeightZNear));

	if(abs( cameraToWorldPos.y ) > cSlopeThreshold )
	{
		float t = HF_HeightFalloff * cameraToWorldPos.y * w;
		fogInt *= (abs( t ) > 0.0001 ? ( ( 1.0 - exp( -t ) ) / t ) : 1.0);
	}
	
	
	
	return exp( -HF_GlobalDensity * w * fogInt );
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
	
	float fog = 1.0f - ComputeVolumetricFog(position, posOriginal);
	float fogDistance = length(posOriginal - HF_CameraPosition);
	float stableFadeEnd = max(HF_WeightZFar, 1000.0f);
	float stableFadeStart = max(HF_WeightZNear, stableFadeEnd * 0.58f);
	float stableWorldFade = smoothstep(stableFadeStart, stableFadeEnd, fogDistance);
	float activeWeatherFog = saturate(AC_RainFXWeight);
	float nightTimeBlend = smoothstep(0.0f, 1.0f, saturate(-AC_LightPos.y * 4.0f))
		* saturate(AC_EnableNightAtmosphere);
	float weatherFogStrength = lerp(1.0f, 1.20f, nightTimeBlend);
	float weatherFog = saturate(max(fog, stableWorldFade) * activeWeatherFog * weatherFogStrength);
	float dryNightFog = fog * nightTimeBlend * (1.0f - activeWeatherFog);
	fog = max(weatherFog, dryNightFog);
	float dryNightCurve = nightTimeBlend * (1.0f - activeWeatherFog);
	float fogSmoother = SmootherStep01(fog);
	float fogLifted = 1.0f - SmootherStep01(1.0f - fog);
	float fogBlendCurve = lerp(fogSmoother, fogLifted, saturate(fog));
	fog = lerp(fog, fogBlendCurve, dryNightCurve);
		
	float3 color = ApplyAtmosphericScatteringGround(position, HF_FogColorMod, true, false);
	float nightFogBrightness = lerp(1.0f, max(0.0f, AC_NightFogBrightness), saturate(AC_EnableNightAtmosphere));
	float3 nightFogColor = float3(0.12f, 0.18f, 0.27f) * nightFogBrightness * 0.8f;
	color = lerp(color, nightFogColor, nightTimeBlend);
	float dayDarknessFactor = max(1.0f, 2.0f - max(0.0f, AC_LightPos.y));
	float darknessFactor = lerp(dayDarknessFactor, 2.0f, nightTimeBlend);
	float maxFogOpacity = lerp(1.0f, 0.85f, nightTimeBlend);
	float rainyNight = activeWeatherFog * nightTimeBlend;
	float rainyNightGeometry = (1.0f - skyPixel) * rainyNight;
	float geometryFogOpacity = lerp(maxFogOpacity, 0.94f, rainyNightGeometry);
	float rainySkyOpacity = lerp(0.54f, 0.90f, nightTimeBlend);
	float skyRainFogAttenuation = lerp(1.0f, rainySkyOpacity, skyPixel * activeWeatherFog);
	float fogOpacity = fog * geometryFogOpacity * skyRainFogAttenuation;
	float rainDepthStart = max(900.0f, stableFadeStart * 0.25f);
	float rainDepthEnd = max(rainDepthStart + 5200.0f, stableFadeEnd * 0.92f);
	float rainDepthT = saturate((fogDistance - rainDepthStart) / max(rainDepthEnd - rainDepthStart, 1.0f));
	float rainDepthRamp = SmootherStep01(rainDepthT);
	float distantRainGeometry = rainyNightGeometry * rainDepthRamp;
	float softSkyRainHaze = skyPixel * rainyNight * 0.75f;
	float softWorldRainHaze = rainyNightGeometry * rainDepthRamp * 0.34f;
	fogOpacity = max(fogOpacity, max(softSkyRainHaze, softWorldRainHaze));

	float3 finalFogColor = saturate(color / darknessFactor);
	float3 midRainGeometryBlue = float3(0.040f, 0.055f, 0.082f);
	float3 farRainGeometryBlue = float3(0.030f, 0.045f, 0.070f);
	float3 rainSkyHazeGray = float3(0.070f, 0.074f, 0.078f);
	float farRainGeometry = saturate(distantRainGeometry * 1.18f);
	float veryFarRainGeometry = rainyNightGeometry * SmootherStep01(saturate((rainDepthT - 0.34f) / 0.66f));
	finalFogColor = lerp(finalFogColor, midRainGeometryBlue, saturate(distantRainGeometry * 0.82f));
	finalFogColor = lerp(finalFogColor, farRainGeometryBlue, saturate(veryFarRainGeometry * 1.08f));
	finalFogColor = lerp(finalFogColor, rainSkyHazeGray, saturate(softSkyRainHaze * 0.72f));
	float farRainLuma = dot(finalFogColor, float3(0.2126f, 0.7152f, 0.0722f));
	float farRainMaxLuma = lerp(1.0f, 0.086f, farRainGeometry);
	farRainMaxLuma = lerp(farRainMaxLuma, 0.070f, saturate(veryFarRainGeometry * 1.18f));
	finalFogColor *= min(1.0f, farRainMaxLuma / max(farRainLuma, 0.001f));

	return float4(finalFogColor, saturate(fogOpacity));
}
