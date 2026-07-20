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
	float WM_IsWaterfall;
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

	// --- Shared depth and shore setup ---
	float surfaceViewZ = Input.vTexcoord2.x;
	float sceneViewZ = LinearizeWaterDepth(TX_Depth.Sample(SS_Linear, screenUV).r);
	float viewRayScale = clamp(abs(Input.vTexcoord2.y) / max(abs(surfaceViewZ), 1.0f), 1.0f, 8.0f);
	float waterThickness = clamp(max(sceneViewZ - surfaceViewZ, 0.0f) * viewRayScale, 0.0f, 6000.0f);
	float shoreDerivative = max(fwidth(waterThickness), 1.0f);
	float shoreFadeEnd = clamp(max(65.0f, shoreDerivative * 1.25f), 65.0f, 160.0f);
	float shoreVisibility = SmootherStep01(saturate((waterThickness - 1.0f) / max(shoreFadeEnd - 1.0f, 1.0f)));

	// --- Shared view and material setup ---
	float3 viewDirection = normalize(Input.vWorldPosition - RI_CameraPosition);
	float waterViewDistance = abs(Input.vTexcoord2.y);
	float isWaterfall = step(0.5f, WM_IsWaterfall);
	float waterMaterialAllowsSSR = (1.0f - step(0.5f, WM_DisableSSR)) * (1.0f - isWaterfall);
	float waterMaterialAllowsRain = 1.0f - step(0.5f, WM_DisableRainEffects);
	float rainAmount = saturate(AC_RainFXWeight) * waterMaterialAllowsRain;
	float nightAmount = saturate((-AC_LightPos.y + 0.12f) * 2.2f);
	float ssrReflectionStrength = max(0.0f, AC_SSRStrength) * step(0.5f, AC_EnableSSR) * waterMaterialAllowsSSR;
	float cubeReflectionStrength = lerp(
		0.30f,
		max(0.30f, saturate(ssrReflectionStrength * 0.82f)),
		step(0.5f, AC_EnableSSR)) * (1.0f - isWaterfall);
	float isLakeWater = step(0.5f, WM_IsOceanWater);
	bool waterSSRActive = ssrReflectionStrength > 0.0001f;

	// --- Shared wave distortion ---
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
	// Re-sample at the corrected UV. A depthless result is sky and must never be
	// used as refracted content below the water surface.
	rawDepthRefracted = TX_Depth.Sample(SS_Linear, distUV).r;
	depthRefracted = LinearizeWaterDepth(rawDepthRefracted);
	float refractedGeometryValid = step(0.000001f, rawDepthRefracted);

	// --- Shared wave normals ---
	float3 wavesFres = normalize(float3(
		distortionBig.x * waveSlopeScale,
		distortionBig.z * 10.0f,
		distortionBig.y * waveSlopeScale));
	float3 wavesSmall = normalize(float3(
		distortionSmall.x * waveSlopeScale,
		distortionSmall.z * 10.0f,
		distortionSmall.y * waveSlopeScale));

	// --- Shared scene sampling and Fresnel ---
	float3 sceneClean = TX_Scene.Sample(SS_Linear, screenUV).rgb;
	float3 sceneRefracted = TX_Scene.Sample(SS_Linear, distUV).rgb;
	float NdotV = saturate(dot(-viewDirection, wavesFres));
	float fresnel = 0.02f + 0.98f * pow(1.0f - NdotV, 5.0f);
	// World-space SSR direction from the water surface into the reflected scene.
	float3 reflectionRayDirection = reflect(viewDirection, wavesFres);
	// Preserve the existing cubemap lookup convention.
	float3 reflectVector = -reflectionRayDirection;
	// Reflections on the top water surface may only originate from the upper
	// hemisphere. This prevents sky below the water plane from becoming SSR or
	// cubemap reflection through steep/distorted wave normals.
	float reflectionHemisphereWeight = smoothstep(0.0f, 0.06f, reflectionRayDirection.y);

	// --- Shared cubemap reflection with weather regrading ---
	// The cubemap predates dynamic weather. Regrade it continuously so it cannot show clear,
	// saturated daytime water during rain or turn the sea electric blue at night.
	float3 reflectionCube = max(TX_ReflectionCube.Sample(SS_Linear, reflectVector).rgb, float3(0.0f, 0.0f, 0.0f));
	float cubeLuma = dot(reflectionCube, float3(0.2126f, 0.7152f, 0.0722f));
	float3 cubeGray = float3(cubeLuma, cubeLuma, cubeLuma);
	float3 rainCloudTint = max(AC_LowCloudRainColor, float3(0.0f, 0.0f, 0.0f));
	float3 dayRainReflection = lerp(cubeGray * 0.46f, float3(0.18f, 0.20f, 0.21f), 0.55f)
		* lerp(float3(1.0f, 1.0f, 1.0f), rainCloudTint, 0.30f);
	float3 clearNightReflection = lerp(reflectionCube * 0.025f, float3(0.004f, 0.009f, 0.023f), 0.72f);
	// Lake water uses subtler rain-night reflections to preserve the Caribbean look.
	float3 rainNightReflection = isLakeWater > 0.5f
		? max(AC_NightRainSkyColor * 0.32f, float3(0.006f, 0.008f, 0.012f))
		: max(AC_NightRainSkyColor * 0.46f, float3(0.012f, 0.018f, 0.030f));
	float3 dayReflection = lerp(reflectionCube, dayRainReflection, rainAmount);
	float3 nightReflection = lerp(clearNightReflection, rainNightReflection, rainAmount);
	float3 fallbackReflection = lerp(dayReflection, nightReflection, nightAmount);

	// --- Shared SSR initialization ---
	float3 reflectionSSR = float3(0.0f, 0.0f, 0.0f);
	float ssrRawWeight = 0.0f;
	float ssrHitQuality = 0.0f;
	float3 ssrHitWorldPosition = Input.vWorldPosition;
	float ssrHitValid = 0.0f;
	float ssrHitIsSky = 0.0f;

	// --- Shared geometry and celestial-free sky SSR raymarching ---
	if (waterSSRActive)
	{
		float3 rayPos = Input.vWorldPosition;
		float3 rayDir = reflectionRayDirection;
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
				// A depthless pixel is sky only for an upward reflection ray. A ray into
				// the lower hemisphere must not reflect sky from below the water surface.
				if (rayDir.y <= 0.0f)
					break;

				// TX_Scene contains the composed sun/moon bodies. For sky SSR, rebuild
				// only the celestial-free atmosphere, then composite low clouds below.
				float3 skyWorldPosition = AC_SpherePosition + AC_CameraPos
					+ normalize(rayDir) * AC_OuterRadius;
				float3 celestialFreeAtmosphere =
					ApplyAtmosphericScatteringSkyWithoutCelestialBodies(skyWorldPosition);
				float3 skyWithoutCelestials = lerp(
					celestialFreeAtmosphere, fallbackReflection, nightAmount);
				float4 reflectedLowClouds = TX_LowClouds.SampleLevel(SS_Linear, uv, 0);
				reflectedLowClouds = ResolveLowCloudLayer(reflectedLowClouds, skyWithoutCelestials);

				reflectionSSR = skyWithoutCelestials * (1.0f - reflectedLowClouds.a)
					+ reflectedLowClouds.rgb;
				ssrHitValid = 1.0f;
				ssrHitIsSky = 1.0f;

				float2 skyEdgeFade = saturate(abs(uv - 0.5f) * 2.0f);
				float skyEdgeDistance = max(skyEdgeFade.x, skyEdgeFade.y);
				ssrRawWeight = 1.0f - smoothstep(0.76f, 1.0f, skyEdgeDistance);
				// Sky remains a valid SSR result even when no low clouds are present.
				ssrHitQuality = 0.72f;
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
				float2 edgeFade = saturate(abs(uv - 0.5f) * 2.0f);
				float edgeDistance = max(edgeFade.x, edgeFade.y);
				ssrRawWeight = 1.0f - smoothstep(0.78f, 1.0f, edgeDistance);
				ssrHitQuality = edgeQuality * nearHitQuality;
				break;
			}
		}
	}


	// --- Shared SSR distance, contact, hit-quality and hemisphere fades ---
	float shallowDepthRefracted = saturate(max(depthRefracted - surfaceViewZ, 0.0f) * 0.01f);
	float ssrNearFade = smoothstep(100.0f, 450.0f, waterViewDistance);
	float ssrContactFade = smoothstep(0.04f, 0.22f, shallowDepthRefracted);
	float ssrBaseWeight = ssrRawWeight
		* lerp(0.45f, 1.0f, ssrNearFade)
		* lerp(0.55f, 1.0f, ssrContactFade)
		* ssrHitQuality
		* reflectionHemisphereWeight;
	float ssrWeight = ssrRawWeight * ssrNearFade * ssrContactFade * ssrHitQuality
		* reflectionHemisphereWeight;

	// --- Shared SSR color processing ---
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

	// ==========================================================================
	// Final water rendering: lake (volumetric Caribbean) vs legacy path
	// ==========================================================================
	float3 finalColor;
	float waterMaskValue;

	if (isLakeWater > 0.5f)
	{
		// ------------------------------------------------------------------
		// Lake water: modern volumetric rendering with absorption & scattering
		// ------------------------------------------------------------------
		float ssrConfidence = saturate(ssrWeight * rainFogVisibility * lerp(1.0f, 0.82f, rainAmount));
		float totalReflectionStrength = lerp(cubeReflectionStrength, ssrReflectionStrength, ssrConfidence);
		float3 reflectionColor = lerp(fallbackReflection, reflectionSSRColor, ssrConfidence);

		// Recalculate the ocean shore masks from the corrected refracted depth.
		float waterThicknessRefracted = clamp(max(depthRefracted - surfaceViewZ, 0.0f) * viewRayScale, 0.0f, 6000.0f);
		float shoreDerivativeRefracted = max(fwidth(waterThicknessRefracted), 1.0f);
		float shoreFadeEndRefracted = clamp(max(65.0f, shoreDerivativeRefracted * 1.25f), 65.0f, 160.0f);
		float shoreVisibilityRefracted = SmootherStep01(saturate((waterThicknessRefracted - 1.0f) / max(shoreFadeEndRefracted - 1.0f, 1.0f)));
		// ---------------------------------------------

		// Depth-dependent absorption keeps shallow sand visible without tinting the whole sea brown.
		float3 clearAbsorption = float3(0.00240f, 0.00115f, 0.00062f);
		float3 rainAbsorption = float3(0.00300f, 0.00155f, 0.00088f);
		float3 transmittance = exp(-lerp(clearAbsorption, rainAbsorption, rainAmount) * waterThicknessRefracted);

		float3 clearDayScattering = float3(0.035f, 0.120f, 0.150f);
		float3 rainDayScattering = float3(0.040f, 0.075f, 0.082f);
		float3 clearNightScattering = float3(0.0070f, 0.0180f, 0.0450f);
		float3 rainNightScattering = max(AC_NightRainSkyColor * 0.65f, float3(0.0090f, 0.0140f, 0.0200f));
		float3 dayScattering = lerp(clearDayScattering, rainDayScattering, rainAmount);
		float3 nightScattering = lerp(clearNightScattering, rainNightScattering, rainAmount);
		float3 scatteringColor = lerp(dayScattering, nightScattering, nightAmount);

		float diffuseLuma = dot(diffuse, float3(0.2126f, 0.7152f, 0.0722f));
		scatteringColor *= lerp(0.94f, 1.06f, saturate(diffuseLuma * 1.4f));
		float3 refractedWaterVolume = sceneRefracted * transmittance
			+ scatteringColor * (1.0f - transmittance);
		// With no geometry behind the water, use the water volume itself. Never use
		// a depthless TX_Scene sky/sun/moon pixel as content seen through the water.
		float3 waterVolume = lerp(scatteringColor, refractedWaterVolume, refractedGeometryValid);
		waterVolume = lerp(sceneClean, waterVolume, shoreVisibilityRefracted);

		// A mild distance blend removes the old horizontal color band without hiding the horizon.
		float farWaterBlend = SmootherStep01(saturate((waterViewDistance - 20000.0f) / 45000.0f)) * shoreVisibilityRefracted;
		float horizonReflectionWeight = saturate(fresnel * totalReflectionStrength * 0.35f)
			* reflectionHemisphereWeight;
		float3 horizonWaterColor = lerp(scatteringColor, fallbackReflection, horizonReflectionWeight);
		waterVolume = lerp(waterVolume, horizonWaterColor, farWaterBlend * 0.38f);

		float oceanTint = saturate(WM_IsOceanWater * WM_OceanWaterTintStrength);
		waterVolume = lerp(
			waterVolume,
			waterVolume * max(WM_OceanWaterTint, float3(0.0f, 0.0f, 0.0f)),
			oceanTint);

		float reflectionAmount = saturate(fresnel * totalReflectionStrength)
			* shoreVisibilityRefracted * reflectionHemisphereWeight;
		float3 color = lerp(waterVolume, reflectionColor, reflectionAmount);

		// Keep sun and moon glints tied to the same reflection control and weather visibility.
		float3 sunOrange = float3(0.6f, 0.3f, 0.1f) * 2.0f;
		float3 sunColor = lerp(sunOrange, float3(1.0f, 1.0f, 1.0f), AC_LightPos.y) * 5.0f;
		float3 reflectVectorSmall = reflect(-viewDirection, wavesSmall);
		float cosSpec = clamp(dot(reflectVectorSmall, -AC_LightPos.xyz), 0.0f, 1.0f);
		float sunSpot = pow(cosSpec, 500.0f) * 0.5f;
		float weatherLightVisibility = GetRainSkyVisibility();
		float sunVisibility = smoothstep(-0.04f, 0.08f, AC_LightPos.y) * weatherLightVisibility;
		sunSpot *= sunVisibility;

		float sunSpotSSRBlock = saturate(max(ssrBaseWeight, ssrHitQuality * 0.75f) * ssrHitValid * 1.85f) * (1.0f - ssrHitIsSky);
		sunSpot *= 1.0f - sunSpotSSRBlock;
		float reflectionControl = saturate(ssrReflectionStrength) * shoreVisibilityRefracted;
		color += sunColor * sunSpot * reflectionControl;

		float moonVisibility = smoothstep(-0.04f, 0.08f, AC_MoonPos.y) * weatherLightVisibility;
		float3 moonLightVector = -AC_MoonPos.xyz;
		float3 moonRayDirection = moonLightVector / max(length(moonLightVector), 0.001f);
		float moonCosSpec = clamp(dot(reflectVectorSmall, moonRayDirection), 0.0f, 1.0f);
		float moonSpot = pow(moonCosSpec, 360.0f) * 0.22f * moonVisibility;
		float moonSSRBlock = sunSpotSSRBlock * (1.0f - ssrHitIsSky);
		moonSpot *= 1.0f - moonSSRBlock;
		float3 moonColor = float3(0.58f, 0.66f, 1.0f) * 1.15f;
		color += moonColor * moonSpot * reflectionControl;

		finalColor = color;
		// Preserve the special rain-disabled mask, while normal water blends cleanly into the shore.
		waterMaskValue = lerp(0.25f * shoreVisibilityRefracted, 1.0f, step(0.5f, WM_DisableRainEffects));
	}
	else
	{
		// ------------------------------------------------------------------
		// Legacy water rendering (all non-lake water)
		// ------------------------------------------------------------------
		float legacyShallowDepth = saturate(max(sceneViewZ - surfaceViewZ, 0.0f) * 0.01f);
		float2 legacyDistUV = screenUV + distortionSmall.xy * DIST_SMALL_AMOUNT + distortionBig.xy * DIST_SMALL_AMOUNT;
		float legacyRawDepthRefracted = TX_Depth.Sample(SS_Linear, legacyDistUV).r;
		float legacyDepthRefracted = LinearizeWaterDepth(legacyRawDepthRefracted);
		legacyDistUV = CleanRefraction(legacyDistUV, screenUV, legacyDepthRefracted);
		legacyDistUV = saturate(legacyDistUV);
		legacyRawDepthRefracted = TX_Depth.Sample(SS_Linear, legacyDistUV).r;
		legacyDepthRefracted = LinearizeWaterDepth(legacyRawDepthRefracted);
		float legacyRefractedGeometryValid = step(0.000001f, legacyRawDepthRefracted);
		float nightDarkening = lerp(1.0f, 2.5f, nightAmount);
		float3 legacyDiffuse = TX_Diffuse.Sample(SS_Linear, Input.vTexcoord + distortionSmall.xy * DIST_SMALL_AMOUNT * 0.5f).rgb / nightDarkening;
		legacyDiffuse = ApplyAtmosphericScatteringGround(Input.vWorldPosition, legacyDiffuse);
		float3 legacyWavesFres = normalize(distortionBig.xzy * float3(1.0f, 10.0f, 1.0f));
		float3 legacyWavesSmall = normalize(distortionSmall.xzy * float3(1.0f, 10.0f, 1.0f));
		float legacyFresnel = min(0.5f, saturate(pow(1.0f - saturate(dot(-viewDirection, legacyWavesFres)), 10.0f)));
		float3 legacySceneSample = TX_Scene.Sample(SS_Linear, legacyDistUV).rgb;
		// A depthless refracted sample is sky. Replace it with the material water
		// color so sun, moon and lower-sky pixels cannot shine through the surface.
		float3 legacyScene = lerp(legacyDiffuse, legacySceneSample, legacyRefractedGeometryValid);
		
		// --- Modern Legacy Water: Murkiness and Foam ---
		float murkiness = saturate(legacyShallowDepth * 0.8f);
		float3 murkColor = float3(0.05f, 0.08f, 0.06f) / nightDarkening;
		murkColor = ApplyAtmosphericScatteringGround(Input.vWorldPosition, murkColor);
		legacyScene = lerp(legacyScene, murkColor, murkiness);
		// -----------------------------------------------------

		float2 legacyCleanUV = lerp(legacyDistUV, screenUV, pow(1.0f - legacyShallowDepth, 20.0f));
		float legacyCleanRawDepth = TX_Depth.Sample(SS_Linear, legacyCleanUV).r;
		float3 legacySceneCleanSample = TX_Scene.Sample(SS_Linear, legacyCleanUV).rgb;
		float3 legacySceneClean = lerp(
			legacyDiffuse,
			legacySceneCleanSample,
			step(0.000001f, legacyCleanRawDepth));
		legacyScene = lerp(legacyScene, legacyDiffuse, 0.73f * max(pow(legacyFresnel, 8.0f), 0.5f));
		float legacySsrFresnel = lerp(0.55f, 0.80f, saturate(pow(1.0f - saturate(dot(-viewDirection, legacyWavesFres)), 2.0f)));
		float legacyCubeWeight = waterSSRActive ? saturate(1.0f - ssrWeight * saturate(ssrReflectionStrength)) : 1.0f;
		float legacyRainCubemapVisibility = lerp(1.0f, 0.12f, rainAmount) * (1.0f - rainAmount * smoothstep(5000.0f, 22000.0f, waterViewDistance));
		legacyScene.rgb += fallbackReflection * cubeReflectionStrength * legacyCubeWeight
			* legacyRainCubemapVisibility * rainFogVisibility * legacyFresnel
			* reflectionHemisphereWeight * lerp(1.0f, legacyDiffuse, 0.6f);
		float legacySsrBlend = saturate(ssrWeight * legacySsrFresnel * ssrReflectionStrength * 0.78f * lerp(0.85f, 1.10f, nightAmount) * rainFogVisibility);
		float legacyPxDistance = Input.vTexcoord2.y;
		float3 legacyColor = lerp(legacyScene, legacySceneClean, pow(saturate(legacyPxDistance / 35000.0f), 4.0f));
		// --- Modern Legacy Water: Shoreline Foam ---
		float shoreFoam = pow(saturate(1.0f - legacyShallowDepth * 4.0f), 2.0f) * saturate(legacyShallowDepth * 20.0f);
		shoreFoam *= saturate(distortionSmall.y + 0.8f);
		float3 foamColor = float3(0.85f, 0.90f, 0.88f) / nightDarkening;
		foamColor = ApplyAtmosphericScatteringGround(Input.vWorldPosition, foamColor);
		legacyColor.rgb = lerp(legacyColor.rgb, foamColor, shoreFoam * 0.85f);
		// -------------------------------------------

		// Dynamic legacy-water shore edge using corrected depth to prevent foreground bleeding.
		float cleanLegacyDepthRefracted = LinearizeWaterDepth(TX_Depth.Sample(SS_Linear, legacyDistUV).r);
		float legacyWaterThickness = clamp(max(cleanLegacyDepthRefracted - surfaceViewZ, 0.0f) * viewRayScale, 0.0f, 6000.0f);
		float legacyShoreDerivative = max(fwidth(legacyWaterThickness), 1.0f);
		float legacyShoreFadeEnd = clamp(max(65.0f, legacyShoreDerivative * 1.25f), 65.0f, 160.0f);
		float legacyShoreVisibility = SmootherStep01(saturate((legacyWaterThickness - 1.0f) / max(legacyShoreFadeEnd - 1.0f, 1.0f)));
		legacyColor.rgb = lerp(sceneClean, legacyColor.rgb, legacyShoreVisibility);

		// Sun glint
		float3 sunOrange = float3(0.6f, 0.3f, 0.1f) * 2.0f;
		float3 sunColor = lerp(sunOrange, float3(1.0f, 1.0f, 1.0f), AC_LightPos.y) * 5.0f;
		float3 legacyReflectVectorSmall = reflect(-viewDirection, legacyWavesSmall);
		float cosSpec = clamp(dot(legacyReflectVectorSmall, -AC_LightPos.xyz), 0.0f, 1.0f);
		float sunSpot = pow(cosSpec, 500.0f) * 0.5f;
		float weatherLightVisibility = GetRainSkyVisibility();
		float sunVisibility = smoothstep(-0.04f, 0.08f, AC_LightPos.y) * weatherLightVisibility;
		sunSpot *= sunVisibility;
		float sunSpotSSRBlock = saturate(max(ssrBaseWeight, ssrHitQuality * 0.75f) * ssrHitValid * 1.85f) * (1.0f - ssrHitIsSky);
		sunSpot *= (1.0f - sunSpotSSRBlock) * legacyShoreVisibility;
		legacyColor.rgb += sunColor * sunSpot;

		// Moon glint
		float moonVisibility = smoothstep(-0.04f, 0.08f, AC_MoonPos.y) * weatherLightVisibility;
		float3 moonLightVector = -AC_MoonPos.xyz;
		float3 moonRayDirection = moonLightVector / max(length(moonLightVector), 0.001f);
		float moonCosSpec = clamp(dot(legacyReflectVectorSmall, moonRayDirection), 0.0f, 1.0f);
		float moonSpot = pow(moonCosSpec, 360.0f) * 0.22f * moonVisibility;
		moonSpot *= (1.0f - sunSpotSSRBlock) * legacyShoreVisibility;
		float3 moonColor = float3(0.58f, 0.66f, 1.0f) * 1.15f;
		legacyColor.rgb += moonColor * moonSpot;

		// Preserve the assembled legacy-water color and its soft night shoreline transition.
		float3 legacyFinalColor = legacyColor;

		legacyFinalColor = lerp(legacyFinalColor, reflectionSSRColor, legacySsrBlend);

		// ------------------------------------------------------------------
		// OWODWAT uses the explicit CPU-side waterfall classification.
		// ------------------------------------------------------------------
		if (isWaterfall > 0.5f)
		{
			// 1. Vertical Flow Distortion (panning noise downwards)
			float2 waterfallUV1 = Input.vTexcoord * 3.0f + float2(0.0f, RI_Time * -0.5f);
			float2 waterfallUV2 = Input.vTexcoord * 5.0f + float2(0.0f, RI_Time * -0.8f);
			
			float3 flowNoise1 = TX_Distortion.Sample(SS_Linear, waterfallUV1).xyz * 2.0f - 1.0f;
			float3 flowNoise2 = TX_Distortion.Sample(SS_Linear, waterfallUV2).xyz * 2.0f - 1.0f;
			float2 flowDistortion = (flowNoise1.xy + flowNoise2.xy) * 0.05f;
			
			// Sample the animated flipbook texture with our organic flow distortion
			float3 waterfallDiffuse = TX_Diffuse.Sample(SS_Linear, Input.vTexcoord + flowDistortion * 0.5f).rgb;
			float diffuseLuma = dot(waterfallDiffuse, float3(0.2126f, 0.7152f, 0.0722f));

			// 2. Steepness / Slope-based Whitewater
			float steepness = 1.0f - saturate(abs(Input.vNormalWS.y));
			
			// 3. Depth-based Foam (where waterfall hits ground/water)
			float contactFoam = pow(saturate(1.0f - legacyShallowDepth * 2.5f), 2.0f); 

			// 4. Subsurface Scattering (Sunlight shining through)
			float sunBacklight = saturate(dot(viewDirection, -AC_LightPos.xyz)); 
			float sssIntensity = pow(sunBacklight, 3.0f) * smoothstep(0.0f, 0.2f, AC_LightPos.y);
			float3 sssColor = float3(0.5f, 0.8f, 0.9f) * sssIntensity * 1.5f;

			// Base color combining elements
			float foamAmount = saturate((diffuseLuma * 1.2f * steepness) + contactFoam * 1.5f);
			float3 waterfallColor = lerp(float3(0.12f, 0.22f, 0.28f), float3(0.85f, 0.95f, 1.0f), foamAmount) / nightDarkening;
			waterfallColor += sssColor * (1.0f - foamAmount);

			// Fog integration
			waterfallColor = ApplyAtmosphericScatteringGround(Input.vWorldPosition, waterfallColor);

			// Opacity & highly distorted refraction
			float edgeFade = pow(saturate(1.0f - dot(-viewDirection, Input.vNormalWS)), 2.0f);
			float opacity = saturate(foamAmount * 0.85f + 0.35f + edgeFade * 0.3f);
			
			float2 refracUV = CleanRefraction(screenUV + flowDistortion * 0.15f, screenUV, legacyDepthRefracted);
			float3 refracScene = TX_Scene.Sample(SS_Linear, saturate(refracUV)).rgb;

			legacyFinalColor = lerp(refracScene, waterfallColor, opacity);
		}

		finalColor = legacyFinalColor;
		waterMaskValue = lerp(
			lerp(0.25f, 1.0f, step(0.5f, WM_DisableRainEffects)),
			1.0f,
			isWaterfall);
	}

	output.color = float4(max(finalColor, float3(0.0f, 0.0f, 0.0f)), 1.0f);
	output.waterMask = waterMaskValue;
	output.fsr3ReactiveMask = 0.45f;
	return output;
}
