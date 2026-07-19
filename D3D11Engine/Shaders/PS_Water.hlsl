//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <AtmosphericScattering.h>
#include <FFFog.h>
#include <DS_Defines.h>

static const float DIST_SMALL_SPEED = -0.01f;
static const float DIST_SMALL_AMOUNT = 0.01f;
static const float DIST_SMALL_SCALE = 0.3f;
static const float DIST_BIG_SCALE = 0.1f;
static const float DIST_BIG_SPEED = -0.005f;


// Cleans the refraction borders
#define CleanRefraction(uv, screen_uv, depthRef) (lerp(uv, screen_uv, saturate(Input.vTexcoord2.x-depthRef)))

cbuffer RefractionInfo : register( b2 )
{
	float4x4 RI_Projection;
	float2 RI_ViewportSize;
	float RI_Time;
	float RI_Pad1;

	float3 RI_CameraPosition;
	float RI_Pad2;

	float4x4 RI_ViewProj;
};

cbuffer WaterMaterialInfo : register( b3 )
{
	float WM_DisableSSR;
	float WM_DisableRainEffects;
	float WM_OceanWaterTintStrength;
	float WM_IsOceanWater;
	float3 WM_OceanWaterTint;
	float WM_Pad;
};

float LinearizeWaterDepth(float rawDepth)
{
	if (rawDepth <= 0.000001f)
		return 1000000.0f;

	float divisor = rawDepth - RI_Projection._33;
	if (abs(divisor) <= 0.000001f)
		return 1000000.0f;

	return RI_Projection._43 / divisor;
}

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Diffuse : register( t0 );

Texture2D	TX_Depth : register( t2 );
TextureCube	TX_ReflectionCube : register( t3 );
Texture2D	TX_Distortion : register( t4 );
Texture2D	TX_Scene : register( t5 );
Texture2D	TX_LowClouds : register( t6 );

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalWS		: TEXCOORD4;
	float3 vWorldPosition	: TEXCOORD5;
	float4 vPosition		: SV_POSITION;
};


//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
struct PS_OUTPUT
{
	float4 color : SV_TARGET0;
	float waterMask : SV_TARGET1;
	float fsr3ReactiveMask : SV_TARGET2;
};

PS_OUTPUT PSMain( PS_INPUT Input )
{
	PS_OUTPUT output;
	float2 screenUV = Input.vPosition.xy / RI_ViewportSize;

	float surfaceViewZ = Input.vTexcoord2.x;
	float rawSceneDepth = TX_Depth.Sample(SS_Linear, screenUV).r;
	float sceneViewZ = LinearizeWaterDepth(rawSceneDepth);
	float viewRayScale = clamp(abs(Input.vTexcoord2.y) / max(abs(surfaceViewZ), 1.0f), 1.0f, 8.0f);
	float shallowDepth = saturate(max(sceneViewZ - surfaceViewZ, 0.0f) * 0.01f);
	float waterThickness = clamp(max(sceneViewZ - surfaceViewZ, 0.0f) * viewRayScale, 0.0f, 6000.0f);
	float shoreDerivative = max(fwidth(waterThickness), 1.0f);
	float shoreFadeEnd = clamp(max(65.0f, shoreDerivative * 1.25f), 65.0f, 160.0f);
	float shoreVisibility = SmootherStep01(saturate((waterThickness - 1.0f) / max(shoreFadeEnd - 1.0f, 1.0f)));

	float3 viewDirection = normalize(Input.vWorldPosition - RI_CameraPosition);
	float waterViewDistance = abs(Input.vTexcoord2.y);
	float waterMaterialAllowsSSR = 1.0f - step(0.5f, WM_DisableSSR);
	float waterMaterialAllowsRain = 1.0f - step(0.5f, WM_DisableRainEffects);
	float rainAmount = saturate(AC_RainFXWeight) * waterMaterialAllowsRain;
	float nightAmount = saturate((-AC_LightPos.y + 0.12f) * 2.2f);
	float ssrReflectionStrength = max(0.0f, AC_SSRStrength) * step(0.5f, AC_EnableSSR) * waterMaterialAllowsSSR;
	float cubeReflectionStrength = lerp(
		0.30f,
		max(0.30f, saturate(ssrReflectionStrength * 0.82f)),
		step(0.5f, AC_EnableSSR));
	bool waterSSRActive = ssrReflectionStrength > 0.0001f;

	// Keep the existing wave animation, but reduce distant repetition and let rain roughen it smoothly.
	float2 worldTexCoord = Input.vWorldPosition.xz / 1000.0f;
	float3 distortionSmall = TX_Distortion.Sample(SS_Linear, worldTexCoord * DIST_SMALL_SCALE + RI_Time * DIST_SMALL_SPEED).xyz * 2.0f - 1.0f;
	distortionSmall += TX_Distortion.Sample(SS_Linear, worldTexCoord * float2(-1.0f, 0.7f) * DIST_SMALL_SCALE + RI_Time * DIST_SMALL_SPEED * 2.0f).xyz * 2.0f - 1.0f;
	distortionSmall *= 0.5f;

	float3 distortionBig = TX_Distortion.Sample(SS_Linear, worldTexCoord * DIST_BIG_SCALE + RI_Time * DIST_BIG_SPEED).xyz * 2.0f - 1.0f;
	distortionBig += TX_Distortion.Sample(SS_Linear, worldTexCoord * float2(-1.0f, 0.7f) * DIST_BIG_SCALE + RI_Time * DIST_BIG_SPEED * 1.2f).xyz * 2.0f - 1.0f;
	distortionBig *= 0.5f;

	float farWaveScale = lerp(1.0f, 0.58f, SmootherStep01(saturate((waterViewDistance - 14000.0f) / 38000.0f)));
	float waveSlopeScale = farWaveScale * lerp(1.0f, 1.12f, rainAmount);
	float refractionDepthFade = SmootherStep01(saturate((waterThickness - 12.0f) / 98.0f));
	float refractionDistortion = shoreVisibility * refractionDepthFade * waveSlopeScale;
	float2 distUV = screenUV + (distortionSmall.xy + distortionBig.xy) * DIST_SMALL_AMOUNT * refractionDistortion;

	float3 diffuse = TX_Diffuse.Sample(
		SS_Linear,
		Input.vTexcoord + distortionSmall.xy * DIST_SMALL_AMOUNT * 0.5f * waveSlopeScale).rgb;

	float rawDepthRefracted = TX_Depth.Sample(SS_Linear, distUV).r;
	float depthRefracted = LinearizeWaterDepth(rawDepthRefracted);
	distUV = CleanRefraction(distUV, screenUV, depthRefracted);
	distUV = saturate(distUV);

	float3 wavesFres = normalize(float3(
		distortionBig.x * waveSlopeScale,
		distortionBig.z * 10.0f,
		distortionBig.y * waveSlopeScale));
	float3 wavesSmall = normalize(float3(
		distortionSmall.x * waveSlopeScale,
		distortionSmall.z * 10.0f,
		distortionSmall.y * waveSlopeScale));

	float3 sceneClean = TX_Scene.Sample(SS_Linear, screenUV).rgb;
	float3 sceneRefracted = TX_Scene.Sample(SS_Linear, distUV).rgb;
	float NdotV = saturate(dot(-viewDirection, wavesFres));
	float fresnel = 0.02f + 0.98f * pow(1.0f - NdotV, 5.0f);
	float3 reflectVector = reflect(-viewDirection, wavesFres);

	// The cubemap predates dynamic weather. Regrade it continuously so it cannot show clear,
	// saturated daytime water during rain or turn the sea electric blue at night.
	float3 reflectionCube = max(TX_ReflectionCube.Sample(SS_Linear, reflectVector).rgb, float3(0.0f, 0.0f, 0.0f));
	float cubeLuma = dot(reflectionCube, float3(0.2126f, 0.7152f, 0.0722f));
	float3 cubeGray = float3(cubeLuma, cubeLuma, cubeLuma);
	float3 rainCloudTint = max(AC_LowCloudRainColor, float3(0.0f, 0.0f, 0.0f));
	float3 dayRainReflection = lerp(cubeGray * 0.46f, float3(0.18f, 0.20f, 0.21f), 0.55f)
		* lerp(float3(1.0f, 1.0f, 1.0f), rainCloudTint, 0.30f);
	float3 clearNightReflection = lerp(reflectionCube * 0.025f, float3(0.004f, 0.009f, 0.023f), 0.72f);
	float3 rainNightReflection = max(AC_NightRainSkyColor * 0.46f, float3(0.012f, 0.018f, 0.030f));
	float3 dayReflection = lerp(reflectionCube, dayRainReflection, rainAmount);
	float3 nightReflection = lerp(clearNightReflection, rainNightReflection, rainAmount);
	float3 fallbackReflection = lerp(dayReflection, nightReflection, nightAmount);

	float3 reflectionSSR = float3(0.0f, 0.0f, 0.0f);
	float ssrRawWeight = 0.0f;
	float ssrHitQuality = 0.0f;
	float3 ssrHitWorldPosition = Input.vWorldPosition;
	float ssrHitValid = 0.0f;
	float ssrHitIsSky = 0.0f;
	float2 ssrHitUV = screenUV;

	if (waterSSRActive)
	{
		float3 rayPos = Input.vWorldPosition;
		float3 rayDir = reflect(viewDirection, wavesFres);
		float stepSize = 40.0f;
		int maxSteps = 40;

		for (int i = 1; i <= maxSteps; i++)
		{
			rayPos += rayDir * stepSize;
			stepSize *= 1.1f;

			float4 projPos = mul(float4(rayPos, 1.0f), RI_ViewProj);
			if (projPos.w <= 0.001f)
				break;

			projPos.xyz /= projPos.w;
			float2 uv = projPos.xy * float2(0.5f, -0.5f) + 0.5f;
			if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f || projPos.z < 0.0f || projPos.z > 1.0f)
				break;

			float rawDepthSample = TX_Depth.SampleLevel(SS_Linear, uv, 0).r;
			if (rawDepthSample <= 0.000001f)
			{
				// Build 132 used the cubemap as stable sky fallback. Keep that behavior and
				// add only low-cloud coverage from screen space, never raw sky/sun pixels.
				float4 skyLowClouds = TX_LowClouds.SampleLevel(SS_Linear, uv, 0);
				if (skyLowClouds.a > 0.001f)
				{
					skyLowClouds = ResolveLowCloudLayer(skyLowClouds, fallbackReflection);
					reflectionSSR = fallbackReflection * (1.0f - skyLowClouds.a) + skyLowClouds.rgb;
					ssrHitValid = 1.0f;
					ssrHitIsSky = 1.0f;
					ssrHitUV = uv;
					float2 skyEdgeFade = saturate(abs(uv - 0.5f) * 2.0f);
					float skyEdgeDistance = max(skyEdgeFade.x, skyEdgeFade.y);
					ssrRawWeight = 1.0f - smoothstep(0.78f, 1.0f, skyEdgeDistance);
					ssrHitQuality = saturate(skyLowClouds.a) * 0.35f;
				}
				break;
			}

			float sampleZ = LinearizeWaterDepth(rawDepthSample);
			float rayZ = projPos.w;
			float depthDiff = rayZ - sampleZ;

			if (depthDiff > 0.0f && depthDiff < (stepSize * 2.0f))
			{
				float3 minPos = rayPos - rayDir * stepSize;
				float3 maxPos = rayPos;
				float3 midPos = rayPos;

				[unroll]
				for (int j = 0; j < 5; j++)
				{
					midPos = (minPos + maxPos) * 0.5f;
					float4 projMid = mul(float4(midPos, 1.0f), RI_ViewProj);
					projMid.xyz /= max(projMid.w, 0.001f);
					float2 uvMid = saturate(projMid.xy * float2(0.5f, -0.5f) + 0.5f);
					float zMid = LinearizeWaterDepth(TX_Depth.SampleLevel(SS_Linear, uvMid, 0).r);

					if (projMid.w - zMid > 0.0f)
						maxPos = midPos;
					else
						minPos = midPos;
				}

				float4 projFinal = mul(float4(midPos, 1.0f), RI_ViewProj);
				projFinal.xyz /= max(projFinal.w, 0.001f);
				uv = saturate(projFinal.xy * float2(0.5f, -0.5f) + 0.5f);

				float2 px = 1.0f / RI_ViewportSize;
				float zL = LinearizeWaterDepth(TX_Depth.SampleLevel(SS_Linear, saturate(uv + float2(-2.0f,  0.0f) * px), 0).r);
				float zR = LinearizeWaterDepth(TX_Depth.SampleLevel(SS_Linear, saturate(uv + float2( 2.0f,  0.0f) * px), 0).r);
				float zU = LinearizeWaterDepth(TX_Depth.SampleLevel(SS_Linear, saturate(uv + float2( 0.0f, -2.0f) * px), 0).r);
				float zD = LinearizeWaterDepth(TX_Depth.SampleLevel(SS_Linear, saturate(uv + float2( 0.0f,  2.0f) * px), 0).r);
				float hitEdge = max(abs(zL - zR), abs(zU - zD));
				float edgeTolerance = max(60.0f, abs(sampleZ) * 0.018f);
				float edgeQuality = 1.0f - smoothstep(edgeTolerance, edgeTolerance * 4.0f, hitEdge);
				float nearHitQuality = smoothstep(700.0f, 2200.0f, abs(sampleZ));

				reflectionSSR = TX_Scene.SampleLevel(SS_Linear, uv, 0).rgb;
				ssrHitWorldPosition = midPos;
				ssrHitValid = 1.0f;
				ssrHitUV = uv;
				float2 edgeFade = saturate(abs(uv - 0.5f) * 2.0f);
				float edgeDistance = max(edgeFade.x, edgeFade.y);
				ssrRawWeight = 1.0f - smoothstep(0.78f, 1.0f, edgeDistance);
				ssrHitQuality = edgeQuality * nearHitQuality;
				break;
			}
		}
	}

	if (ssrHitValid > 0.5f && ssrHitIsSky < 0.5f)
	{
		float4 reflectedLowClouds = TX_LowClouds.SampleLevel(SS_Linear, ssrHitUV, 0);
		reflectedLowClouds = ResolveLowCloudLayer(reflectedLowClouds, reflectionSSR);
		reflectionSSR = reflectionSSR * (1.0f - reflectedLowClouds.a) + reflectedLowClouds.rgb;
	}

	float ssrNearFade = smoothstep(100.0f, 450.0f, waterViewDistance);
	float ssrContactFade = smoothstep(0.04f, 0.22f, shallowDepth);
	float ssrBaseWeight = ssrRawWeight
		* lerp(0.45f, 1.0f, ssrNearFade)
		* lerp(0.55f, 1.0f, ssrContactFade)
		* ssrHitQuality;
	float ssrWeight = ssrRawWeight * ssrNearFade * ssrContactFade * ssrHitQuality;

	float3 reflectionSSRColor = max(reflectionSSR, float3(0.0f, 0.0f, 0.0f));
	float rainFogVisibility = 1.0f;
	if (ssrHitValid > 0.5f && ssrHitIsSky < 0.5f)
	{
		float3 rainAtmosphereColor = ApplyAtmosphericScatteringGround(ssrHitWorldPosition, reflectionSSRColor);
		reflectionSSRColor = lerp(reflectionSSRColor, rainAtmosphereColor, rainAmount);

		float hitDistance = length(ssrHitWorldPosition - AC_WorldCameraPos);
		float rainFogOcclusion = rainAmount * smoothstep(5000.0f, 22000.0f, hitDistance);
		rainFogVisibility = 1.0f - rainFogOcclusion;
		rainFogVisibility *= rainFogVisibility;
	}

	float reflectionLuma = dot(reflectionSSRColor, float3(0.2126f, 0.7152f, 0.0722f));
	reflectionSSRColor *= rcp(1.0f + max(0.0f, reflectionLuma - 6.0f) * 0.12f);
	float ssrFresnel = lerp(0.55f, 0.80f, saturate(pow(1.0f - NdotV, 2.0f)));
	float ssrBlend = saturate(ssrWeight * ssrFresnel * ssrReflectionStrength * 0.78f * lerp(0.85f, 1.10f, nightAmount) * rainFogVisibility);
	float3 reflectionColor = fallbackReflection;

	float legacyShallowDepth = saturate(max(sceneViewZ - surfaceViewZ, 0.0f) * 0.01f);
	float2 legacyDistUV = screenUV + distortionSmall.xy * DIST_SMALL_AMOUNT + distortionBig.xy * DIST_SMALL_AMOUNT;
	float legacyRawDepthRefracted = TX_Depth.Sample(SS_Linear, legacyDistUV).r;
	float legacyDepthRefracted = LinearizeWaterDepth(legacyRawDepthRefracted);
	legacyDistUV = CleanRefraction(legacyDistUV, screenUV, legacyDepthRefracted);
	legacyDistUV = saturate(legacyDistUV);
	float3 legacyDiffuse = TX_Diffuse.Sample(SS_Linear, Input.vTexcoord + distortionSmall.xy * DIST_SMALL_AMOUNT * 0.5f).rgb;
	float3 legacyWavesFres = normalize(distortionBig.xzy * float3(1.0f, 10.0f, 1.0f));
	float3 legacyWavesSmall = normalize(distortionSmall.xzy * float3(1.0f, 10.0f, 1.0f));
	float legacyFresnel = min(0.5f, saturate(pow(1.0f - saturate(dot(-viewDirection, legacyWavesFres)), 10.0f)));
	float3 legacyScene = TX_Scene.Sample(SS_Linear, legacyDistUV).rgb;
	float oceanSkyThroughGuard = saturate(WM_IsOceanWater);
	legacyScene = lerp(legacyScene, lerp(reflectionCube, legacyScene, step(0.000001f, legacyRawDepthRefracted)), oceanSkyThroughGuard);
	float3 legacySceneClean = TX_Scene.Sample(SS_Linear, lerp(legacyDistUV, screenUV, pow(1.0f - legacyShallowDepth, 20.0f))).rgb;
	float legacyWetFactor = 1.0f - saturate(pow(1.0f - legacyShallowDepth, 8.0f) + clamp(pow(distortionSmall.y, 2.0f), 0.5f, 1.0f));
	float legacyEnhancedWater = step(0.5f, AC_EnableSSR) * waterMaterialAllowsSSR;
	float3 legacySceneWet = lerp(legacySceneClean, legacySceneClean * lerp(0.01f, 0.38f, nightAmount * legacyEnhancedWater), legacyWetFactor);
	legacyScene = lerp(legacyScene, legacyScene * float3(4.0f, 0.2f, 0.1f) * 0.05f, legacyWetFactor);
	legacyScene = lerp(legacyScene, legacyDiffuse, 0.73f * max(pow(legacyFresnel, 8.0f), 0.5f));
	float legacySsrFresnel = lerp(0.55f, 0.80f, saturate(pow(1.0f - saturate(dot(-viewDirection, legacyWavesFres)), 2.0f)));
	float legacyCubeWeight = waterSSRActive ? saturate(1.0f - ssrWeight * saturate(ssrReflectionStrength)) : 1.0f;
	float legacyRainCubemapVisibility = lerp(1.0f, 0.12f, rainAmount) * (1.0f - rainAmount * smoothstep(5000.0f, 22000.0f, waterViewDistance));
	legacyScene.rgb += reflectionCube * cubeReflectionStrength * legacyCubeWeight * legacyRainCubemapVisibility * legacyFresnel * lerp(1.0f, legacyDiffuse, 0.6f);
	float legacySsrBlend = saturate(ssrWeight * legacySsrFresnel * ssrReflectionStrength * 0.78f * lerp(0.85f, 1.10f, nightAmount) * rainFogVisibility);
	float legacyPxDistance = Input.vTexcoord2.y;
	float3 legacyColor = lerp(legacyScene, legacySceneClean, pow(saturate(legacyPxDistance / 35000.0f), 4.0f));
	legacyColor = lerp(legacyColor, legacySceneWet, 1.0f - legacyShallowDepth);
	legacyColor.rgb = ApplyAtmosphericScatteringGround(Input.vWorldPosition, legacyColor.rgb);
	float3 sunOrange = float3(0.6f, 0.3f, 0.1f) * 2.0f;
	float3 sunColor = lerp(sunOrange, float3(1.0f, 1.0f, 1.0f), AC_LightPos.y) * 5.0f;
	float3 legacyReflectVectorSmall = reflect(-viewDirection, legacyWavesSmall);
	float cosSpec = clamp(dot(legacyReflectVectorSmall, -AC_LightPos.xyz), 0.0f, 1.0f);
	float sunSpot = pow(cosSpec, 500.0f) * 0.5f;
	float weatherLightVisibility = GetRainSkyVisibility();
	float sunVisibility = smoothstep(-0.04f, 0.08f, AC_LightPos.y) * weatherLightVisibility;
	sunSpot *= sunVisibility;
	float sunSpotSSRBlock = saturate(max(ssrBaseWeight, ssrHitQuality * 0.75f) * ssrHitValid * 1.85f);
	sunSpot *= 1.0f - sunSpotSSRBlock;
	legacyColor.rgb += sunColor * sunSpot;
	float moonVisibility = smoothstep(-0.04f, 0.08f, AC_MoonPos.y) * weatherLightVisibility;
	float3 moonLightVector = -AC_MoonPos.xyz;
	float3 moonRayDirection = moonLightVector / max(length(moonLightVector), 0.001f);
	float moonCosSpec = clamp(dot(legacyReflectVectorSmall, moonRayDirection), 0.0f, 1.0f);
	float moonSpot = pow(moonCosSpec, 360.0f) * 0.22f * moonVisibility;
	moonSpot *= 1.0f - sunSpotSSRBlock;
	float3 moonColor = float3(0.58f, 0.66f, 1.0f) * 1.15f;
	legacyColor.rgb += moonColor * moonSpot;
	float legacyDarknessFactor = 2.0f - AC_LightPos.y;
	legacyDarknessFactor = lerp(legacyDarknessFactor, max(1.22f, legacyDarknessFactor * 0.58f), nightAmount * legacyEnhancedWater);
	float3 legacyFinalColor = legacyColor / max(legacyDarknessFactor, 0.001f);
	legacyFinalColor = lerp(legacyFinalColor, reflectionSSRColor, legacySsrBlend);
	float oceanTint = saturate(WM_IsOceanWater * WM_OceanWaterTintStrength);
	if (oceanTint > 0.001f)
	{
		// Ocean finish only. All water uses the shared Build-132 SSR path above.
		float3 clearAbsorption = float3(0.00240f, 0.00115f, 0.00062f);
		float3 rainAbsorption = float3(0.00300f, 0.00155f, 0.00088f);
		float3 oceanTransmittance = exp(-lerp(clearAbsorption, rainAbsorption, rainAmount) * waterThickness);
		float3 clearDayScattering = float3(0.035f, 0.120f, 0.150f);
		float3 rainDayScattering = float3(0.052f, 0.080f, 0.096f);
		float3 clearNightScattering = float3(0.0035f, 0.0080f, 0.0200f);
		float3 rainNightScattering = max(AC_NightRainMidColor * 1.05f + AC_NightRainSkyColor * 0.20f, float3(0.018f, 0.030f, 0.052f));
		float3 oceanScattering = lerp(lerp(clearDayScattering, rainDayScattering, rainAmount), lerp(clearNightScattering, rainNightScattering, rainAmount), nightAmount);
		oceanScattering *= lerp(0.94f, 1.06f, saturate(dot(legacyDiffuse, float3(0.2126f, 0.7152f, 0.0722f)) * 1.4f));
		float oceanRefractedValid = step(0.000001f, legacyRawDepthRefracted);
		float3 oceanScene = lerp(reflectionCube, TX_Scene.Sample(SS_Linear, legacyDistUV).rgb, oceanRefractedValid);
		float3 oceanVolume = oceanScene * oceanTransmittance + oceanScattering * (1.0f - oceanTransmittance);
		oceanVolume = lerp(sceneClean, oceanVolume, shoreVisibility);
		float farWaterBlend = SmootherStep01(saturate((waterViewDistance - 20000.0f) / 45000.0f)) * shoreVisibility;
		float oceanNdotV = saturate(dot(-viewDirection, legacyWavesFres));
		float oceanFresnel = 0.02f + 0.98f * pow(1.0f - oceanNdotV, 5.0f);
		float horizonReflectionWeight = saturate(oceanFresnel * cubeReflectionStrength * 0.35f);
		float3 horizonWaterColor = lerp(oceanScattering, fallbackReflection, horizonReflectionWeight);
		oceanVolume = lerp(oceanVolume, horizonWaterColor, farWaterBlend * 0.38f);
		oceanVolume = oceanVolume * max(WM_OceanWaterTint, float3(0.0f, 0.0f, 0.0f));
		float rainWaterCoverage = lerp(0.42f, 1.0f, shoreVisibility);
		float rainWaterRegradeWeight = rainAmount * rainWaterCoverage * lerp(0.68f, 0.90f, nightAmount);
		float3 dayRainWaterHaze = float3(0.072f, 0.092f, 0.100f);
		float3 nightRainWaterHaze = max(AC_NightRainMidColor * 1.28f + AC_NightRainSkyColor * 0.34f, float3(0.020f, 0.034f, 0.058f));
		float3 rainWaterHaze = lerp(dayRainWaterHaze, nightRainWaterHaze, nightAmount);
		float oceanLuma = dot(oceanVolume, float3(0.2126f, 0.7152f, 0.0722f));
		float3 rainRegradedOcean = rainWaterHaze * lerp(0.72f, 1.24f, saturate(oceanLuma * 4.0f));
		oceanVolume = lerp(oceanVolume, rainRegradedOcean, rainWaterRegradeWeight);
		float shoreFinishWeight = saturate(shoreVisibility * lerp(0.40f, 0.72f, nightAmount) + farWaterBlend * 0.35f);
		legacyFinalColor = lerp(legacyFinalColor, oceanVolume, oceanTint * shoreFinishWeight);
	}
	else
	{
		legacyFinalColor = lerp(legacyFinalColor, legacyFinalColor * max(WM_OceanWaterTint, float3(0.0f, 0.0f, 0.0f)), oceanTint);
	}
	output.color = float4(max(legacyFinalColor, float3(0.0f, 0.0f, 0.0f)), 1.0f);
	output.waterMask = lerp(0.25f, 1.0f, step(0.5f, WM_DisableRainEffects));
	output.fsr3ReactiveMask = 0.45f;
	return output;
}
