//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

#include <AtmosphericScattering.h>


//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Texture0 : register( t0 );
Texture2D	TX_Texture1 : register( t1 );
Texture2D	TX_Texture2 : register( t2 );
Texture2D	TX_RainCloudBase : register( t3 );
Texture2D	TX_RainCloudDetail : register( t4 );


//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vWorldPosition	: TEXCOORD5;
	float4 vPosition		: SV_POSITION;
};

float AtmosphereDither(float2 pixelPosition)
{
	float n1 = frac(52.9829189f * frac(dot(pixelPosition, float2(0.06711056f, 0.00583715f))));
	float n2 = frac(52.9829189f * frac(dot(pixelPosition + 37.17f, float2(0.00583715f, 0.06711056f))));
	return n1 + n2 - 1.0f;
}

//--------------------------------------------------------------------------------------
float3 ApplyMoonTexture(float3 worldPosition)
{
    float3 localPosition = worldPosition - AC_SpherePosition;
    float3 skyDirection = normalize(localPosition - AC_CameraPos);
    float3 moonDirection = normalize(AC_MoonPos);

    float3 referenceUp = abs(moonDirection.y) > 0.98f
        ? float3(0.0f, 0.0f, 1.0f)
        : float3(0.0f, 1.0f, 0.0f);
    float3 moonRight = normalize(cross(referenceUp, moonDirection));
    float3 moonUp = normalize(cross(moonDirection, moonRight));

    float forward = dot(skyDirection, moonDirection);
    float2 tangentPosition = float2(
        dot(skyDirection, moonRight),
        dot(skyDirection, moonUp)) / max(0.001f, forward);

    const float moonAngularHalfSize = 0.08f;
    float2 moonUV = float2(0.5f, 0.5f) +
        tangentPosition / (moonAngularHalfSize * 2.0f);
    moonUV.y = 1.0f - moonUV.y;

    float2 inside = step(0.0f, moonUV) * step(moonUV, 1.0f);
    float boundsMask = inside.x * inside.y * step(0.0f, forward);
    float4 moonTexture = TX_Texture2.Sample(SS_Linear, saturate(moonUV));
    float moonLuminance = max(moonTexture.r, max(moonTexture.g, moonTexture.b));
    float textureMask = smoothstep(0.005f, 0.03f, moonLuminance);
    float moonMask = boundsMask * textureMask * AC_MoonVisibility * GetRainSkyVisibility();
    return moonTexture.rgb * moonMask;
}

float4 RenderRainCloudDeck(float3 worldPosition)
{
	float3 skyDirection = normalize(worldPosition - AC_SpherePosition);
	float horizonMask = smoothstep(0.0f, 0.065f, skyDirection.y);

	// Repeat the original Gothic cloud deck across the dome instead of sampling
	// one almost constant texture patch over the entire upper sky.
	float2 upperWind = float2(0.00035f, 0.00018f) * AC_Time;
	float2 upperParallax = AC_WorldCameraPos.xz * 0.0000015f;
	float2 upperUV = skyDirection.xz * 3.2f + upperWind + upperParallax + float2(0.17f, 0.43f);
	float3 upperSample = TX_RainCloudBase.Sample(SS_Linear, upperUV).rgb;
	float upperLuma = dot(upperSample, float3(0.2126f, 0.7152f, 0.0722f));
	float upperStructure = smoothstep(0.16f, 0.60f, upperLuma);

	float2 lightPlane = normalize(AC_LightPos.xz + float2(0.001f, 0.001f));
	float shiftedLuma = dot(
		TX_RainCloudBase.Sample(SS_Linear, upperUV + lightPlane * 0.035f).rgb,
		float3(0.2126f, 0.7152f, 0.0722f));
	float upperRelief = saturate(0.48f + (upperLuma - shiftedLuma) * 4.5f);

	// The transparent detail bank uses a rotated, denser projection, faster wind
	// and stronger camera parallax so it reads as a separate lower cloud layer.
	float2 lowerWind = float2(0.00105f, -0.00052f) * AC_Time;
	float2 lowerParallax = AC_WorldCameraPos.xz * 0.0000045f;
	float2 lowerDirection = float2(
		skyDirection.x * 0.67f + skyDirection.z * 0.18f,
		skyDirection.z - skyDirection.x * 0.12f);
	float2 lowerUV = frac(lowerDirection * 5.1f + lowerWind + lowerParallax + float2(0.31f, 0.12f));
	float4 lowerSample = TX_RainCloudDetail.Sample(SS_Linear, lowerUV);
	float lowerAlpha = smoothstep(0.025f, 0.68f, lowerSample.a) * horizonMask;
	float lowerLuma = dot(lowerSample.rgb, float3(0.2126f, 0.7152f, 0.0722f));
	float lowerStructure = smoothstep(0.10f, 0.62f, lowerLuma);

	float dayWeight = saturate(AC_LightPos.y * 4.0f + 0.10f);
	float3 upperDay = lerp(float3(0.070f, 0.080f, 0.092f), float3(0.46f, 0.48f, 0.50f), upperStructure);
	upperDay *= lerp(0.78f, 1.16f, upperRelief);
	float3 lowerDay = lerp(float3(0.045f, 0.052f, 0.062f), float3(0.35f, 0.38f, 0.41f), lowerStructure);
	float3 upperNight = lerp(float3(0.005f, 0.009f, 0.019f), float3(0.070f, 0.090f, 0.135f), upperStructure);
	float3 lowerNight = lerp(float3(0.003f, 0.006f, 0.013f), float3(0.045f, 0.060f, 0.095f), lowerStructure);
	float3 upperColor = lerp(upperNight, upperDay, dayWeight);
	float3 lowerColor = lerp(lowerNight, lowerDay, dayWeight);
	lowerColor += float3(0.12f, 0.16f, 0.25f) * AC_MoonVisibility * lowerStructure * 0.08f;

	float upperOpacity = saturate((0.92f + upperStructure * 0.07f) * horizonMask);
	float3 cloudColor = lerp(upperColor, lowerColor, lowerAlpha * 0.90f);
	float opacity = saturate(upperOpacity + lowerAlpha * (1.0f - upperOpacity));
	return float4(cloudColor, opacity);
}
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float3 atmoColor = ApplyAtmosphericScatteringSky(Input.vWorldPosition) * 2.0f;
	
	float4 clouds = TX_Texture0.Sample(SS_Linear, 0.5f + Input.vWorldPosition.xz / 200000.0f + frac(AC_Time * 0.001f));
	float4 night = TX_Texture1.Sample(SS_Linear, 0.5f + Input.vWorldPosition.xz / 200000.0f + frac(AC_Time * 0.0001f));
	//float cloudsAlpha = TX_Texture0.SampleLevel(SS_Linear, Input.vWorldPosition.xz / 700000.0f + frac(AC_Time * 0.001f), 5).a;
	//atmoColor = lerp(atmoColor, clouds.r * lerp(atmoColor, 1.0f, 0.5f), cloudsAlpha / 2);
	
	clouds.rgb *= lerp(atmoColor, 1.0f, saturate(AC_LightPos.y));
	night.rgb = lerp(0.0f, night, saturate(-AC_LightPos.y * 4)); // Make sure stars are only visible at night
	night.rgb *= GetRainSkyVisibility(); // Dense rain clouds fully occlude stars.
	
	
	atmoColor += ApplyMoonTexture(Input.vWorldPosition);

	float rainCloudWeight = smoothstep(0.02f, 0.55f, saturate(AC_RainFXWeight));
	float clearCloudWeight = 1.0f - rainCloudWeight * 0.85f;
	atmoColor = lerp(atmoColor, clouds.rgb, clouds.a * 0.4f * clearCloudWeight);

	[branch] if (rainCloudWeight > 0.001f)
	{
		float4 rainClouds = RenderRainCloudDeck(Input.vWorldPosition);
		atmoColor = lerp(atmoColor, rainClouds.rgb, rainClouds.a * rainCloudWeight);
	}
	
	// Apply stars
	atmoColor += night * 0.4f;
	
	atmoColor = saturate(atmoColor + AtmosphereDither(Input.vPosition.xy) * (GetNightWeight() * 1.5f / 255.0f));
	return float4(atmoColor,1);
}

