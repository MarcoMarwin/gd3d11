/** Atmospheric scattering header */
#ifndef ATMOSPHERIC_SCATTERING_H_
#define ATMOSPHERIC_SCATTERING_H_

static const float NIGHT_BRIGHTNESS = 2.0f;

cbuffer Atmosphere : register( b1 )
{
	float AC_Kr4PI;
	float AC_Km4PI;	
	float AC_g;
	float AC_KrESun;

	float AC_KmESun;
	float AC_InnerRadius;
	float AC_OuterRadius;
	float AC_Scale;

	float3 AC_Wavelength;
	float AC_RayleighScaleDepth;


	float AC_RayleighOverScaleDepth;
	int AC_nSamples;
	float AC_fSamples;
	float AC_CameraHeight;

	float3 AC_CameraPos;
	float AC_Time;
	float3 AC_LightPos;
	float AC_SceneWettness;

	float3 AC_MoonPos;
	float AC_MoonVisibility;

	float3 AC_SpherePosition;
	float AC_RainFXWeight;

	float AC_EnableSSR;
	float AC_EnableSSS;
	float AC_SSRStrength;
	float AC_SSSIntensity;

	float AC_WaterCubemapStrength;
	float AC_EnableNightAtmosphere;
	float AC_NearNightBrightness;
	float AC_NightFogBrightness;

	float AC_NightDarkeningStart;
	float AC_NightDarkeningMax;
	float AC_NightDarkeningRange;
	float AC_SunVisibility;

	float3 AC_WorldCameraPos;
	float AC_EnableContactShadows;

	float AC_EnableScreenSpaceGI;
	float AC_SkyEffectsEnabled;
	float AC_ContactShadowStrength;
	float AC_ScreenSpaceGIStrength;

	float AC_EnableParticleLighting;
	float AC_ParticleLightingStrength;
	float AC_PadParticle0;
	float AC_PadParticle1;

	float3 AC_NightRainMidColor;
	float AC_NightRainWorldHazeStrength;
	float3 AC_NightRainFarColor;
	float AC_NightRainMidInfluence;
	float3 AC_NightRainSkyColor;
	float AC_NightRainSkyHazeStrength;
	float AC_NightRainFarMaxLuma;
	float AC_NightRainVeryFarMaxLuma;
	float AC_NightRainVeryFarInfluence;
	float AC_DayRainAtmosphereStrength;

	float3 AC_LowCloudDayColor;
	float AC_LowCloudDensity;
	float3 AC_LowCloudRainColor;
	float AC_LowCloudScale;
	float3 AC_LowCloudNightColor;
	float AC_LowCloudSpeed;
	float AC_LowCloudHeightScale;
	float AC_LowCloudDistanceScale;
	float AC_LowCloudSunLight;
	float AC_LowCloudPad0;

	float4 AC_LightScreenPos;
};

// The scale equation calculated by Vernier's Graphical Analysis
float AC_Escale(float fCos)
{
	float x = 1.0 - fCos;
	return AC_RayleighScaleDepth * exp(-0.00287 + x*(0.459 + x*(3.83 + x*(-6.80 + x*5.25))));
}
// Calculates the Mie phase function
float AC_getMiePhase(float fCos, float fCos2, float g, float g2)
{
	return 1.5 * ((1.0 - g2) / (2.0 + g2)) * (1.0 + fCos2) / pow(abs(1.0 + g2 - 2.0*g*fCos), 1.5);
}
// Calculates the Rayleigh phase function
float AC_getRayleighPhase(float fCos2)
{
	//return 1.0;
	return 0.75 + 0.75*fCos2;
}
// Returns the near intersection point of a line and a sphere
float AC_getNearIntersection(float3 v3Pos, float3 v3Ray, float fDistance2, float fRadius2)
{
	float B = 2.0 * dot(v3Pos, v3Ray);
	float C = fDistance2 - fRadius2;
	float fDet = max(0.0, B*B - 4.0 * C);
	return 0.5 * (-B - sqrt(fDet));
}
// Returns the far intersection point of a line and a sphere
float AC_getFarIntersection(float3 v3Pos, float3 v3Ray, float fDistance2, float fRadius2)
{
	float B = 2.0 * dot(v3Pos, v3Ray);
	float C = fDistance2 - fRadius2;
	float fDet = max(0.0, B*B - 4.0 * C);
	return 0.5 * (-B + sqrt(fDet));
}

float3 GetAtmosphericSunTerm(float3 normal)
{
	return saturate(dot(normal, AC_LightPos));
}

float SmootherStep01(float x)
{
	x = saturate(x);
	return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
}

#if defined(ENABLE_LOW_CLOUDS)
float LowCloudHash21(float2 p)
{
	p = frac(p * float2(123.34f, 456.21f));
	p += dot(p, p + 45.32f);
	return frac(p.x * p.y);
}

float LowCloudValueNoise(float2 p)
{
	float2 i = floor(p);
	float2 f = frac(p);
	float2 u = f * f * (3.0f - 2.0f * f);

	float a = LowCloudHash21(i);
	float b = LowCloudHash21(i + float2(1.0f, 0.0f));
	float c = LowCloudHash21(i + float2(0.0f, 1.0f));
	float d = LowCloudHash21(i + float2(1.0f, 1.0f));

	return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float LowCloudFbm(float2 p)
{
	float n = LowCloudValueNoise(p) * 0.58f;
	n += LowCloudValueNoise(p * 2.07f + 19.17f) * 0.28f;
	n += LowCloudValueNoise(p * 4.13f + 7.31f) * 0.14f;
	return n;
}

float LowCloudHash31(float3 p)
{
	p = frac(p * 0.1031f);
	p += dot(p, p.yzx + 33.33f);
	return frac((p.x + p.y) * p.z);
}

float LowCloudValueNoise3(float3 p)
{
	float3 i = floor(p);
	float3 f = frac(p);
	float3 u = f * f * (3.0f - 2.0f * f);

	float n000 = LowCloudHash31(i + float3(0.0f, 0.0f, 0.0f));
	float n100 = LowCloudHash31(i + float3(1.0f, 0.0f, 0.0f));
	float n010 = LowCloudHash31(i + float3(0.0f, 1.0f, 0.0f));
	float n110 = LowCloudHash31(i + float3(1.0f, 1.0f, 0.0f));
	float n001 = LowCloudHash31(i + float3(0.0f, 0.0f, 1.0f));
	float n101 = LowCloudHash31(i + float3(1.0f, 0.0f, 1.0f));
	float n011 = LowCloudHash31(i + float3(0.0f, 1.0f, 1.0f));
	float n111 = LowCloudHash31(i + float3(1.0f, 1.0f, 1.0f));

	float nx00 = lerp(n000, n100, u.x);
	float nx10 = lerp(n010, n110, u.x);
	float nx01 = lerp(n001, n101, u.x);
	float nx11 = lerp(n011, n111, u.x);
	float nxy0 = lerp(nx00, nx10, u.y);
	float nxy1 = lerp(nx01, nx11, u.y);
	return lerp(nxy0, nxy1, u.z);
}

float LowCloudFbm3(float3 p)
{
	float n = LowCloudValueNoise3(p) * 0.62f;
	n += LowCloudValueNoise3(p * 2.03f + 17.11f) * 0.28f;
	n += LowCloudValueNoise3(p * 4.01f + 61.73f) * 0.10f;
	return n;
}

float ResolveWorldLowCloudBase(float baseFogHeight)
{
	return baseFogHeight;
}

void GetWorldLowCloudLight(out float3 lightDir, out float lightWeight)
{
	float sunWeight = saturate(AC_SunVisibility) * smoothstep(0.04f, 0.42f, AC_LightPos.y);
	float moonWeight = saturate(AC_MoonVisibility) * saturate(AC_EnableNightAtmosphere) * smoothstep(0.02f, 0.34f, AC_MoonPos.y) * 0.34f;
	float useMoon = step(sunWeight, moonWeight);
	float3 sunDir = normalize(lerp(float3(-0.25f, 0.72f, 0.18f), AC_LightPos, saturate(abs(AC_LightPos.y) + 0.12f)));
	float3 moonDir = normalize(lerp(float3(0.22f, 0.64f, -0.28f), AC_MoonPos, saturate(abs(AC_MoonPos.y) + 0.12f)));
	lightDir = normalize(lerp(sunDir, moonDir, useMoon));
	lightWeight = max(sunWeight, moonWeight);
}

float ComputeWorldLowCloudDensity(float3 worldPosition, float baseFogHeight)
{
	float cloudBase = ResolveWorldLowCloudBase(baseFogHeight);
	float cloudScale = max(0.35f, AC_LowCloudScale);
	float invCloudScale = 1.0f / cloudScale;
	float cloudHeightScale = max(0.35f, AC_LowCloudHeightScale);
	float cloudSpeed = max(0.0f, AC_LowCloudSpeed);
	float3 wind = float3(AC_Time * 3.8f, AC_Time * 0.04f, -AC_Time * 2.3f) * cloudSpeed;
	float3 macroP = (worldPosition + wind * 52.0f) * float3(0.000026f, 0.000032f, 0.000026f) * invCloudScale;
	float3 warpP = (worldPosition + wind * 34.0f) * float3(0.000040f, 0.000047f, 0.000040f) * invCloudScale;
	float3 warp = float3(
		LowCloudValueNoise3(warpP + float3(13.1f, 2.7f, 41.9f)),
		LowCloudValueNoise3(warpP + float3(57.7f, 19.3f, 8.2f)),
		LowCloudValueNoise3(warpP + float3(4.8f, 63.4f, 27.5f))) * 2.0f - 1.0f;
	float3 warpedWorld = worldPosition + warp * float3(14800.0f, 3600.0f, 14800.0f) * cloudScale;

	float macro = LowCloudFbm3((warpedWorld + wind * 50.0f) * float3(0.000024f, 0.000030f, 0.000024f) * invCloudScale);
	float body = LowCloudFbm3((warpedWorld + wind * 30.0f) * float3(0.000104f, 0.000118f, 0.000104f) * invCloudScale + float3(19.3f, 4.7f, 71.1f));
	float torn = LowCloudFbm3((warpedWorld + wind * 16.0f) * float3(0.000190f, 0.000170f, 0.000190f) * invCloudScale + float3(43.0f, 12.0f, 5.0f));
	float topNoise = LowCloudValueNoise3(macroP * 1.08f + float3(77.0f, 9.0f, 23.0f));
	float baseNoise = LowCloudValueNoise3(macroP * 0.82f + float3(12.0f, 51.0f, 6.0f));

	float islands = smoothstep(0.43f, 0.73f, macro + body * 0.18f);
	float broadGaps = smoothstep(0.50f, 0.82f, LowCloudValueNoise3(macroP * 1.72f + float3(31.0f, 7.0f, 91.0f)) + torn * 0.12f);
	float bodyCore = smoothstep(0.38f, 0.75f, body * 0.78f + macro * 0.36f - torn * 0.12f);
	float brokenBody = islands * lerp(bodyCore, bodyCore * 0.30f, broadGaps * 0.68f);
	float cloudBody = SmootherStep01(brokenBody);

	float localBase = cloudBase + (lerp(500.0f, 2300.0f, baseNoise) - broadGaps * 900.0f) * cloudHeightScale;
	float localTop = cloudBase + (lerp(8200.0f, 15800.0f, topNoise) + islands * 2100.0f + cloudBody * 1700.0f - broadGaps * 1900.0f) * cloudHeightScale;
	float lowCenter = cloudBase + (3200.0f + (baseNoise - 0.5f) * 900.0f) * cloudHeightScale;
	float highCenter = lerp(cloudBase + 6500.0f * cloudHeightScale, localTop - 2600.0f * cloudHeightScale, saturate(islands * 0.86f + bodyCore * 0.22f));

	float lowerCore = 1.0f - smoothstep(1500.0f * cloudHeightScale, 5200.0f * cloudHeightScale, abs(worldPosition.y - lowCenter));
	float lowerSkirt = 1.0f - smoothstep(3400.0f * cloudHeightScale, 9800.0f * cloudHeightScale, abs(worldPosition.y - (localBase + 2800.0f * cloudHeightScale)));
	float highBank = 1.0f - smoothstep(2800.0f * cloudHeightScale, 8700.0f * cloudHeightScale, abs(worldPosition.y - highCenter));
	float bottomFade = smoothstep(localBase - 1200.0f * cloudHeightScale, localBase + 2400.0f * cloudHeightScale, worldPosition.y);
	float topFeather = lerp(3200.0f, 6200.0f, topNoise) * cloudHeightScale;
	float topFade = 1.0f - smoothstep(localTop - topFeather, localTop + topFeather * 0.85f, worldPosition.y);
	float verticalBand = saturate(lowerCore * 1.14f + lowerSkirt * 0.40f + highBank * 0.50f) * bottomFade * topFade;

	return saturate(verticalBand * cloudBody * 1.42f * max(0.0f, AC_LowCloudDensity));
}

float4 ComputeWorldLowCloudVolume(float3 cameraWorld, float3 endWorld, float cameraDistance, float skyPixel, float baseFogHeight, float3 fogColorMod, float nightTimeBlend)
{
	float3 ray = endWorld - cameraWorld;
	float rayDistance = max(length(ray), 1.0f);
	float3 rayDir = ray / rayDistance;
	float cloudBase = ResolveWorldLowCloudBase(baseFogHeight);
	float cloudHeightScale = max(0.35f, AC_LowCloudHeightScale);
	float cloudDistanceScale = max(0.45f, AC_LowCloudDistanceScale);
	float layerMin = cloudBase - 3200.0f * cloudHeightScale;
	float layerMax = cloudBase + 18200.0f * cloudHeightScale;
	float marchDistance = lerp(min(cameraDistance, 105000.0f), 78000.0f, skyPixel) * cloudDistanceScale;
	float startDistance = lerp(7000.0f, 14500.0f, skyPixel) * cloudDistanceScale;
	float skyHorizonWeight = lerp(1.0f, lerp(0.12f, 1.0f, 1.0f - SmootherStep01(saturate((rayDir.y - 0.20f) / 0.52f))), skyPixel);
	if (abs(rayDir.y) > 0.035f)
	{
		float t0 = (layerMin - cameraWorld.y) / rayDir.y;
		float t1 = (layerMax - cameraWorld.y) / rayDir.y;
		float layerEnter = min(t0, t1);
		float layerExit = max(t0, t1);
		startDistance = max(startDistance, layerEnter - 3600.0f * cloudHeightScale);
		marchDistance = min(marchDistance, layerExit + 5200.0f * cloudHeightScale);
	}
	if (marchDistance <= startDistance + 200.0f)
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	float usableDistance = max(marchDistance - startDistance, 1.0f);
	float dayWeight = saturate(AC_LightPos.y * 2.4f + 0.22f);
	float rainWeight = saturate(AC_RainFXWeight * lerp(max(0.0f, AC_DayRainAtmosphereStrength), 1.0f, saturate((-AC_LightPos.y) * 10.0f)));
	float3 dayLitClear = lerp(fogColorMod * 0.70f, float3(0.78f, 0.79f, 0.76f), 0.70f) * max(AC_LowCloudDayColor, float3(0.0f, 0.0f, 0.0f));
	float3 dayShadowClear = lerp(fogColorMod * 0.38f, float3(0.38f, 0.40f, 0.40f), 0.68f) * max(AC_LowCloudDayColor, float3(0.0f, 0.0f, 0.0f));
	float3 dayLitRain = lerp(fogColorMod * 0.58f, float3(0.54f, 0.55f, 0.54f), 0.72f) * max(AC_LowCloudRainColor, float3(0.0f, 0.0f, 0.0f));
	float3 dayShadowRain = lerp(fogColorMod * 0.27f, float3(0.21f, 0.23f, 0.24f), 0.80f) * max(AC_LowCloudRainColor, float3(0.0f, 0.0f, 0.0f));
	float3 dayLit = lerp(dayLitClear, dayLitRain, rainWeight);
	float3 dayShadow = lerp(dayShadowClear, dayShadowRain, rainWeight);
	float3 nightLit = float3(0.035f, 0.044f, 0.060f) * max(AC_LowCloudNightColor, float3(0.0f, 0.0f, 0.0f));
	float3 nightShadow = float3(0.012f, 0.018f, 0.028f) * max(AC_LowCloudNightColor, float3(0.0f, 0.0f, 0.0f));

	float3 lightDir = normalize(lerp(float3(-0.25f, 0.72f, 0.18f), AC_LightPos, saturate(abs(AC_LightPos.y) + 0.12f)));
	float transmittance = 1.0f;
	float3 scattering = 0.0f;
	float accumulatedAlpha = 0.0f;
	const int CLOUD_FIELD_STEPS = 8;
	int activeCloudSteps = (rainWeight > 0.25f || nightTimeBlend > 0.70f) ? 5 : CLOUD_FIELD_STEPS;
	float stepLength = usableDistance / max((float)activeCloudSteps, 1.0f);

	[loop]
	for (int i = 0; i < CLOUD_FIELD_STEPS; ++i)
	{
		if (i >= activeCloudSteps)
		{
			break;
		}
		float stepJitter = LowCloudHash21(float2(i * 17.0f, i * 29.0f) + float2(11.0f, 37.0f)) - 0.5f;
		float sampleDistance = startDistance + (i + 0.58f + stepJitter * 0.24f) * stepLength;
		float3 sampleWorld = cameraWorld + rayDir * sampleDistance;
		float nearCloudFadeStart = lerp(7800.0f, 18000.0f, skyPixel) * cloudDistanceScale;
		float nearCloudFadeEnd = lerp(18000.0f, 32000.0f, skyPixel) * cloudDistanceScale;
		float farCloudFadeStart = lerp(105000.0f, 72000.0f, skyPixel) * cloudDistanceScale;
		float farCloudFadeEnd = lerp(140000.0f, 98000.0f, skyPixel) * cloudDistanceScale;
		float distanceFade = smoothstep(nearCloudFadeStart, nearCloudFadeEnd, sampleDistance) * (1.0f - smoothstep(farCloudFadeStart, farCloudFadeEnd, sampleDistance));
		float density = ComputeWorldLowCloudDensity(sampleWorld, baseFogHeight) * distanceFade * skyHorizonWeight;

		float upperSelfLight = smoothstep(cloudBase + 2600.0f * cloudHeightScale, cloudBase + 9400.0f * cloudHeightScale, sampleWorld.y);
		float selfShadow = lerp(0.46f, 0.94f, upperSelfLight) * lerp(1.0f, 0.72f, saturate(density * 1.20f));
		float3 litColor = lerp(nightLit, dayLit, dayWeight);
		float3 shadowColor = lerp(nightShadow, dayShadow, dayWeight);
		float3 cloudColor = lerp(shadowColor, litColor, selfShadow);
		float upperSunLayer = smoothstep(cloudBase + 3900.0f * cloudHeightScale, cloudBase + 9200.0f * cloudHeightScale, sampleWorld.y);
		float viewSunForward = pow(saturate(dot(rayDir, lightDir) * 0.5f + 0.5f), 4.0f);
		float sunTopLight = upperSunLayer * dayWeight * saturate(lightDir.y * 1.35f) * selfShadow * lerp(1.0f, 0.45f, rainWeight) * max(0.0f, AC_LowCloudSunLight);
		cloudColor += float3(0.155f, 0.156f, 0.140f) * sunTopLight * lerp(0.38f, 0.88f, viewSunForward);

		float sampleAlpha = saturate(density * lerp(0.53f, 0.75f, dayWeight) * lerp(1.0f, 0.92f, nightTimeBlend));
		sampleAlpha = 1.0f - exp(-sampleAlpha * stepLength * 0.00022f);
		float weight = sampleAlpha * transmittance;
		scattering += cloudColor * weight;
		accumulatedAlpha += weight;
		transmittance *= 1.0f - sampleAlpha;
	}

	accumulatedAlpha = saturate(accumulatedAlpha * 1.04f);
	return float4(saturate(scattering / max(accumulatedAlpha, 0.001f)), accumulatedAlpha);
}

float ComputeWorldLowCloudShadow(float3 worldPosition, float baseFogHeight, float nightTimeBlend)
{
	return 0.0f;
}

float ComputeWorldLowCloudGlobalShadow(float baseFogHeight, float nightTimeBlend)
{
	return 0.0f;
}
#endif // ENABLE_LOW_CLOUDS

float GetNightWeight()
{
	return saturate((-AC_LightPos.y) * 10.0f);
}

float GetRainSkyVisibility()
{
	float rainOcclusion = smoothstep(0.05f, 0.65f, saturate(AC_RainFXWeight));
	return 1.0f - rainOcclusion;
}

float GetNightDistanceFade(float3 worldPosition)
{
	float cameraDistance = length(worldPosition - AC_WorldCameraPos);
	float nightFadeStart = max(0.0f, AC_NightDarkeningStart);
	float nightFadeEnd = nightFadeStart + max(1000.0f, AC_NightDarkeningRange);
	float nightDistanceBlend = SmootherStep01((cameraDistance - nightFadeStart) / max(1.0f, nightFadeEnd - nightFadeStart));
	return nightDistanceBlend * GetNightWeight() * saturate(AC_EnableNightAtmosphere);
}

float3 ApplyNightDistanceDarkening(float3 worldPosition, float3 color)
{
	float nightDistanceFade = GetNightDistanceFade(worldPosition);
	float3 farNightColor = float3(0.0012f, 0.0016f, 0.0035f);
	float baseNightDarkening = saturate(AC_NightDarkeningMax);
	float extraNightDarkening = saturate(AC_NightDarkeningMax - 1.0f);
	color = lerp(color, farNightColor, nightDistanceFade * baseNightDarkening);
	return lerp(color, float3(0.0f, 0.0f, 0.0f), nightDistanceFade * extraNightDarkening);
}

float3 ApplyAtmosphericScatteringGround(float3 worldPosition, float3 in_color, bool applyNightshade=true, bool applyDistanceDarkening=true)
{
	float3 camPos = AC_CameraPos;
	float3 v3Pos = worldPosition - AC_SpherePosition;
	float3 v3Ray = v3Pos - camPos;

	float nightWeight = GetNightWeight();
		
	float innerRadius = AC_InnerRadius;
				
	const int iSamples = 1;
	const int fSamples = iSamples;
				
	// Get the ray from the camera to the vertex, and its length (which is the far point of the ray passing through the atmosphere)
	float fFar = length(v3Ray);
	v3Ray /= fFar;

	//if(AC_CameraHeight > AC_OuterRadius)
	//	return in_color;
	
	// Calculate the ray's starting position, then calculate its scattering offset
	float3 v3Start = camPos;
	float fDepth = exp((innerRadius - AC_CameraHeight) / AC_RayleighScaleDepth);
	float fCameraAngle = max(1.0f, dot(-v3Ray, v3Pos) / length(v3Pos));
	float fLightAngle = dot(AC_LightPos, v3Pos) / length(v3Pos);
	float fCameraScale = AC_Escale(fCameraAngle);
	float fLightScale = AC_Escale(fLightAngle);
	float fCameraOffset = fDepth*fCameraScale;
	float fTemp = (fLightScale + fCameraScale);

	// Initialize the scattering loop variables
	float fSampleLength = fFar / fSamples;
	float fScaledLength = fSampleLength * AC_Scale;
	float3 v3SampleRay = v3Ray * fSampleLength;
	float3 v3SamplePoint = v3Start + v3SampleRay * 0.5;

	float3 vInvWavelength = 1.0f / pow(AC_Wavelength, 4.0f);
	
	// Now loop through the sample rays
	float3 v3FrontColor = float3(0.0, 0.0, 0.0);
	float3 v3Attenuate;
	for(int i=0; i<iSamples; i++)
	{
		float fHeight = length(v3SamplePoint);
		float fDepth = exp(AC_RayleighOverScaleDepth * (innerRadius - fHeight));
		float fScatter = fDepth*fTemp - fCameraOffset;
		v3Attenuate = exp(-fScatter * (vInvWavelength * AC_Kr4PI + AC_Km4PI));
		v3FrontColor += v3Attenuate * (fDepth * fScaledLength);
		v3SamplePoint += v3SampleRay;
	}
	// Suppress daytime atmospheric in-scattering during rain, as in the established renderer path.
	// Distant-world color follows the monotonic atmospheric rain envelope, not wet-ground persistence.
	v3FrontColor *= 1.0f - saturate(AC_RainFXWeight);
	
	// Finally, scale the Mie and Rayleigh colors and set up the varying variables for the pixel shader.
	float3 c0 = v3FrontColor * (vInvWavelength * AC_KrESun + AC_KmESun);
	// Fade residual daytime scattering only at night and only toward the far-distance boundary.
	float nightDistanceFade = GetNightDistanceFade(worldPosition);
	c0 *= 1.0f - nightDistanceFade;
	//c0 = lerp(dot(float3(0.333f,0.333f,0.333f), c0), c0, 0.5f);
	float3 c1 = v3Attenuate;
	
	float3 dayColor = c0 + in_color * c1;
	float nearNightBrightness = lerp(1.0f, max(0.0f, AC_NearNightBrightness), saturate(AC_EnableNightAtmosphere));
	float3 nightColor = float3(0.095f,0.115f,0.255f) * NIGHT_BRIGHTNESS * nearNightBrightness;
	float moonWeight = saturate((-AC_LightPos.y - 0.08f) * 1.7f);
	float midtone = saturate(dot(in_color, float3(0.299f, 0.587f, 0.114f)) * 0.95f + 0.04f);
	float3 moonColor = float3(0.018f, 0.026f, 0.052f) * moonWeight * midtone * nearNightBrightness;
	float3 outColor;

	if(applyNightshade)
		outColor = dayColor + in_color * nightColor * nightWeight + moonColor;
	else
		outColor = dayColor + nightColor * nightWeight + moonColor;

	return applyDistanceDarkening ? ApplyNightDistanceDarkening(worldPosition, outColor) : outColor;
}

float3 ApplyAtmosphericScatteringSky(float3 worldPosition)
{
	float3 camPos = AC_CameraPos;
	float3 vPos = (worldPosition) - AC_SpherePosition;
	float3 vRay = vPos - camPos;
				
	float fFar = length(vRay);
	vRay /= fFar;
	
	//return float4(abs(AC_SpherePosition), 1);
	
	//if(AC_CameraHeight < AC_InnerRadius)
	//	return float4(1,0,0,1);
	
	// Calculate the closest intersection of the ray with the outer atmosphere (which is the near point of the ray passing through the atmosphere)
	float fNear = AC_getNearIntersection(camPos, vRay, AC_CameraHeight * AC_CameraHeight, AC_OuterRadius * AC_OuterRadius);

	// Calculate the ray's starting position, then calculate its scattering offset
	float3 vStart = camPos;

	float fHeight = length(vStart);
	float fDepth = exp(AC_RayleighOverScaleDepth * (AC_InnerRadius - AC_CameraHeight));
	float fStartAngle = dot(vRay, vStart) / fHeight;
	float fStartOffset = fDepth*AC_Escale(fStartAngle);
	
	// Initialize the scattering loop variables
	float fSampleLength = fFar / AC_fSamples;
	float fScaledLength = fSampleLength * AC_Scale;
	float3 vSampleRay = vRay * fSampleLength;
	float3 vSamplePoint = vStart + vSampleRay * 0.5;
	
	float3 vInvWavelength = 1.0f / pow(AC_Wavelength, 4.0f);
	
	//return retF(AC_InnerRadius - length(vSamplePoint));
	
	// Now loop through the sample rays
	float3 vFrontColor = float3(0.0, 0.0, 0.0);
	for(int i=0; i<AC_nSamples; i++)
	{
		float fHeight = length(vSamplePoint);
		float fDepth = exp(AC_RayleighOverScaleDepth * (AC_InnerRadius - fHeight));
		float fLightAngle = dot(AC_LightPos, vSamplePoint) / fHeight;
		float fCameraAngle = dot(vRay, vSamplePoint) / fHeight;
		float fScatter = (fStartOffset + fDepth*(AC_Escale(fLightAngle) - AC_Escale(fCameraAngle)));
		
		float3 vAttenuate = exp(-fScatter * (vInvWavelength * AC_Kr4PI + AC_Km4PI));
		
		vFrontColor += vAttenuate * (fDepth * fScaledLength);
		vSamplePoint += vSampleRay;
	}
	
	float yCLip = saturate(1-pow(1-vPos.y, 20.0f));
	
	// Finally, scale the Mie and Rayleigh colors and set up the varying variables for the pixel shader
	float3 c0 = vFrontColor * (vInvWavelength * AC_KrESun);
	// Rain clouds occlude only the concentrated Mie sun glow; the base sky transition stays intact.
	float3 c1 = vFrontColor * AC_KmESun * GetRainSkyVisibility();
	
	
	
	float3 vDirection = camPos - vPos;
	
	float fCos = dot(AC_LightPos, vDirection) / length(vDirection);
	
	float fCos2 = fCos*fCos;

	float3 color = AC_getRayleighPhase(fCos2) * c0 + AC_getMiePhase(fCos, fCos2, AC_g, AC_g * AC_g) * c1 * 2.0f;
	
	return color;
}

float3 ApplyAtmosphericScatteringOuter(float3 worldPosition)
{
	float3 camPos = AC_CameraPos;
	float3 vPos = (worldPosition) - AC_SpherePosition;
	float3 vRay = vPos - camPos;
				
	float fFar = length(vRay);
	vRay /= fFar;
	
	//return float4(abs(AC_SpherePosition), 1);
	
	//if(AC_CameraHeight < AC_InnerRadius)
	//	return float4(1,0,0,1);
	
	// Calculate the closest intersection of the ray with the outer atmosphere (which is the near point of the ray passing through the atmosphere)
	float fNear = AC_getNearIntersection(camPos, vRay, AC_CameraHeight * AC_CameraHeight, AC_OuterRadius * AC_OuterRadius);

	// Calculate the ray's starting position, then calculate its scattering offset
	float3 vStart = camPos + vRay * fNear;
	fFar -= fNear;

	float fStartAngle = dot(vRay, vStart) / AC_OuterRadius;
	float fStartDepth = exp(-1.0 / AC_RayleighScaleDepth);
	float fStartOffset = fStartDepth*AC_Escale(fStartAngle);
	
	// Initialize the scattering loop variables
	float fSampleLength = fFar / AC_fSamples;
	float fScaledLength = fSampleLength * AC_Scale;
	float3 vSampleRay = vRay * fSampleLength;
	float3 vSamplePoint = vStart + vSampleRay * 0.5;
	
	float3 vInvWavelength = 1.0f / pow(AC_Wavelength, 4.0f);
	
	//return retF(AC_InnerRadius - length(vSamplePoint));
	
	// Now loop through the sample rays
	float3 vFrontColor = float3(0.0, 0.0, 0.0);
	for(int i=0; i<AC_nSamples; i++)
	{
		float fHeight = length(vSamplePoint);
		float fDepth = exp(AC_RayleighOverScaleDepth * (AC_InnerRadius - fHeight));
		float fLightAngle = dot(AC_LightPos, vSamplePoint) / fHeight;
		float fCameraAngle = dot(vRay, vSamplePoint) / fHeight;
		float fScatter = (fStartOffset + fDepth*(AC_Escale(fLightAngle) - AC_Escale(fCameraAngle)));
		
		float3 vAttenuate = exp(-fScatter * (vInvWavelength * AC_Kr4PI + AC_Km4PI));
		
		vFrontColor += vAttenuate * (fDepth * fScaledLength);
		vSamplePoint += vSampleRay;
	}
	
	// Finally, scale the Mie and Rayleigh colors and set up the varying variables for the pixel shader
	float3 c0 = vFrontColor * (vInvWavelength * AC_KrESun) * 2.0f;
	
	float3 c1 = vFrontColor * AC_KmESun;	
	float3 vDirection = camPos - vPos;
	
	float fCos = dot(AC_LightPos, vDirection) / length(vDirection);
	
	float fCos2 = fCos*fCos;

	float3 color = AC_getRayleighPhase(fCos2) * c0 + AC_getMiePhase(fCos, fCos2, AC_g, AC_g * AC_g)* c1;
	
	return color;
}

#endif
