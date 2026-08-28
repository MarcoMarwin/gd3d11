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
Texture2D	TX_RainCloud : register( t3 );


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

//--------------------------------------------------------------------------------------
float GetMoonTextureMask(float3 worldPosition)
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
    return boundsMask * textureMask * GetRainSkyVisibility();
}

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
    float moonMask = GetMoonTextureMask(worldPosition);
    return moonTexture.rgb * moonMask;
}

float4 RenderRainCloudDeck(float3 worldPosition)
{
	float3 skyDirection = normalize(worldPosition - AC_SpherePosition);
	float horizonMask = smoothstep(0.0f, 0.085f, skyDirection.y);

	// One broad, seamless Gothic rain cloud deck. The stronger rain fog now
	// supplies depth; a second fast detail layer looked tiled and artificial.
	float2 wind = float2(0.00028f, 0.00014f) * AC_Time;
	float2 parallax = AC_WorldCameraPos.xz * 0.0000012f;
	float2 cloudUV = skyDirection.xz * 1.55f + wind + parallax + float2(0.17f, 0.43f);
	float3 sample0 = TX_RainCloud.Sample(SS_Linear, cloudUV).rgb;
	float luma = dot(sample0, float3(0.2126f, 0.7152f, 0.0722f));
	float structure = smoothstep(0.18f, 0.62f, luma);

	float2 lightPlane = normalize(AC_LightPos.xz + float2(0.001f, 0.001f));
	float shiftedLuma = dot(
		TX_RainCloud.Sample(SS_Linear, cloudUV + lightPlane * 0.030f).rgb,
		float3(0.2126f, 0.7152f, 0.0722f));
	float relief = saturate(0.58f + (luma - shiftedLuma) * 2.2f);

	float dayWeight = saturate(AC_LightPos.y * 4.0f + 0.10f);
	float3 dayCloud = lerp(float3(0.24f, 0.25f, 0.25f), float3(0.46f, 0.47f, 0.47f), structure) * lerp(0.88f, 1.06f, relief);
	float3 nightCloud = lerp(float3(0.012f, 0.017f, 0.027f), float3(0.070f, 0.088f, 0.120f), structure);
	float3 cloudColor = lerp(nightCloud, dayCloud, dayWeight);
	cloudColor += float3(0.10f, 0.13f, 0.19f) * AC_MoonVisibility * structure * 0.035f;

	float opacity = saturate((0.98f + structure * 0.02f) * horizonMask);
	return float4(cloudColor, opacity);
}
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float3 atmoColor = ApplyAtmosphericScatteringSky(Input.vWorldPosition) * 2.0f;
	
	float4 clouds = TX_Texture0.Sample(SS_Linear, 0.5f + Input.vWorldPosition.xz / 200000.0f + frac(AC_Time * 0.001f));
	float4 night = TX_Texture1.Sample(SS_Linear, 0.5f + Input.vWorldPosition.xz / 200000.0f + frac(AC_Time * 0.0001f));
	clouds.rgb *= lerp(atmoColor, 1.0f, saturate(AC_LightPos.y));
	night.rgb = lerp(0.0f, night, saturate(-AC_LightPos.y * 4)); // Make sure stars are only visible at night
	night.rgb *= GetRainSkyVisibility(); // Dense rain clouds fully occlude stars.
	float moonSkyMask = GetMoonTextureMask(Input.vWorldPosition);
	night.rgb *= 1.0f - moonSkyMask;
	// Apply stars before the moon; the moon mask prevents stars from leaking through the lunar disk.
	atmoColor += night.rgb * 0.4f;
	atmoColor += ApplyMoonTexture(Input.vWorldPosition);

	float rainCloudWeight = GetRainCloudTransitionWeight();
	float clearCloudWeight = 1.0f - rainCloudWeight * 0.85f;

	atmoColor = lerp(atmoColor, clouds.rgb, clouds.a * 0.4f * clearCloudWeight);

	[branch]
	if (rainCloudWeight > 0.001f)
	{
		float4 rainClouds = RenderRainCloudDeck(Input.vWorldPosition);
		atmoColor = lerp(atmoColor, rainClouds.rgb, rainClouds.a * rainCloudWeight);
	}
	
	return float4(atmoColor,1);
}
