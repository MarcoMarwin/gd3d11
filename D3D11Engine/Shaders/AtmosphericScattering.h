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

float ComputeWorldLowCloudDensity(float3 worldPosition, float baseFogHeight, float weatherWeight)
{
	float lowerShelf = 1.0f - smoothstep(650.0f, 3900.0f, abs(worldPosition.y - (baseFogHeight + 650.0f)));
	float upperShelf = 1.0f - smoothstep(1300.0f, 6100.0f, abs(worldPosition.y - (baseFogHeight + 2850.0f)));
	float valleyFloor = 1.0f - smoothstep(3600.0f, 11800.0f, max(0.0f, worldPosition.y - (baseFogHeight + 1200.0f)));
	float verticalBand = saturate((lowerShelf * 1.05f + upperShelf * 0.55f) * lerp(0.78f, 1.12f, valleyFloor));

	float2 wind = float2(AC_Time * 3.2f, -AC_Time * 1.7f);
	float2 broadUV = (worldPosition.xz + wind * 42.0f) * 0.000155f;
	float broadNoise = LowCloudFbm(broadUV);
	float tornNoise = LowCloudFbm(broadUV * 2.35f + float2(13.4f, 71.8f));
	float detailNoise = LowCloudValueNoise(broadUV * 7.7f + float2(3.1f, 41.7f));
	float ridgeBreakup = sin(worldPosition.x * 0.00052f + worldPosition.z * 0.00039f + broadNoise * 5.1f) * 0.5f + 0.5f;

	float coverageSeed = broadNoise + tornNoise * 0.28f + ridgeBreakup * 0.12f - detailNoise * 0.08f;
	float coverage = smoothstep(0.32f, lerp(0.78f, 0.62f, weatherWeight), coverageSeed);
	float tornEdge = smoothstep(0.12f, 0.88f, tornNoise + detailNoise * 0.18f);
	return saturate(verticalBand * coverage * lerp(0.72f, 1.0f, tornEdge));
}

float4 ComputeWorldLowCloudVolume(float3 cameraWorld, float3 endWorld, float cameraDistance, float skyPixel, float baseFogHeight, float3 fogColorMod, float nightTimeBlend)
{
	float skyEffects = saturate(AC_SkyEffectsEnabled);
	float weatherWeight = saturate(AC_RainFXWeight);
	float atmosphericWeight = skyEffects * lerp(0.34f, 1.0f, weatherWeight);
	if (atmosphericWeight <= 0.0001f)
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	float3 ray = endWorld - cameraWorld;
	float rayDistance = max(length(ray), 1.0f);
	float3 rayDir = ray / rayDistance;
	float marchDistance = lerp(min(cameraDistance, 68000.0f), 62000.0f, skyPixel);
	float startDistance = lerp(900.0f, 2200.0f, skyPixel);
	if (skyPixel < 0.5f && marchDistance <= startDistance + 50.0f)
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}
	float usableDistance = max(marchDistance - startDistance, 1.0f);

	float3 dryColor = lerp(fogColorMod * 0.30f, float3(0.064f, 0.076f, 0.080f), 0.74f);
	float3 rainColor = float3(0.042f, 0.050f, 0.057f);
	float3 nightColor = float3(0.012f, 0.017f, 0.030f);
	float3 cloudColor = lerp(dryColor, rainColor, weatherWeight);
	cloudColor = lerp(cloudColor, nightColor, nightTimeBlend);

	float3 lightDir = normalize(lerp(float3(-0.22f, 0.78f, 0.18f), AC_LightPos, saturate(abs(AC_LightPos.y) + 0.15f)));
	float transmittance = 1.0f;
	float3 scattering = 0.0f;
	float accumulatedAlpha = 0.0f;
	const int CLOUD_MARCH_STEPS = 7;
	float stepLength = usableDistance / CLOUD_MARCH_STEPS;

	[unroll]
	for (int i = 0; i < CLOUD_MARCH_STEPS; ++i)
	{
		float stepJitter = LowCloudHash21(float2(i + 17.0f, floor(AC_Time * 0.021f))) - 0.5f;
		float sampleDistance = startDistance + (i + 0.55f + stepJitter * 0.18f) * stepLength;
		float3 sampleWorld = cameraWorld + rayDir * sampleDistance;
		float distanceFade = smoothstep(2400.0f, 10500.0f, sampleDistance) * (1.0f - smoothstep(76000.0f, 104000.0f, sampleDistance));
		float density = ComputeWorldLowCloudDensity(sampleWorld, baseFogHeight, weatherWeight) * distanceFade;

		float terrainContact = 1.0f - smoothstep(0.0f, 3200.0f, abs(sampleDistance - marchDistance));
		density *= lerp(1.0f, 0.55f, terrainContact * (1.0f - skyPixel));

		float shadowDensity = ComputeWorldLowCloudDensity(sampleWorld + lightDir * lerp(1600.0f, 2900.0f, weatherWeight), baseFogHeight, weatherWeight);
		float selfShadow = lerp(1.0f, 0.38f, saturate(shadowDensity * lerp(0.45f, 0.85f, weatherWeight)));
		float nightOcclusion = lerp(1.0f, 0.58f, nightTimeBlend * weatherWeight);
		float skyVisibility = lerp(1.0f, 0.40f, skyPixel);

		float sampleAlpha = saturate(density * atmosphericWeight * lerp(0.22f, 0.42f, weatherWeight) * lerp(0.92f, 1.28f, nightTimeBlend) * skyVisibility);
		sampleAlpha = 1.0f - exp(-sampleAlpha * stepLength * 0.00022f);
		float weight = sampleAlpha * transmittance;
		float shade = selfShadow * nightOcclusion;
		scattering += cloudColor * lerp(0.62f, 1.0f, shade) * weight;
		accumulatedAlpha += weight;
		transmittance *= 1.0f - sampleAlpha;
	}

	float horizonBody = smoothstep(9000.0f, 28000.0f, marchDistance) * weatherWeight * nightTimeBlend;
	accumulatedAlpha = saturate(accumulatedAlpha + horizonBody * 0.10f * (1.0f - skyPixel * 0.45f) * atmosphericWeight);
	return float4(saturate(scattering / max(accumulatedAlpha, 0.001f)), accumulatedAlpha);
}

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
