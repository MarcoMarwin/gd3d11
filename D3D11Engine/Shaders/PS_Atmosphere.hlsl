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
	float projectionScale = min(4.0f, rcp(max(skyDirection.y, 0.16f)));
	float2 cloudPlane = skyDirection.xz * projectionScale;

	float2 lowWind = float2(0.00016f, 0.00009f) * AC_Time;
	float2 highWind = float2(-0.00007f, 0.00012f) * AC_Time;
	float2 baseUV = float2(0.17f, 0.43f) + cloudPlane * 0.105f + lowWind;
	float2 warpUV = float2(0.61f, 0.29f) + cloudPlane * 0.31f + highWind;
	float2 warp = float2(
		TX_RainCloudDetail.Sample(SS_Linear, warpUV).r,
		TX_RainCloudDetail.Sample(SS_Linear, warpUV * 0.93f + float2(0.37f, 0.61f)).r) - 0.5f;
	float2 warpedUV = baseUV + warp * 0.075f;

	float lowLayer = TX_RainCloudBase.Sample(SS_Linear, warpedUV).r;
	float2 rotatedPlane = float2(-cloudPlane.y, cloudPlane.x);
	float highLayer = TX_RainCloudBase.Sample(
		SS_Linear, float2(0.73f, 0.11f) + rotatedPlane * 0.071f + highWind).r;
	float detail = TX_RainCloudDetail.Sample(
		SS_Linear, warpedUV * 3.2f - lowWind * 1.7f + float2(0.19f, 0.47f)).r;
	float density = saturate(lowLayer * 0.61f + highLayer * 0.39f + (detail - 0.5f) * 0.34f);

	float lightPlanarLength = max(length(AC_LightPos.xz), 0.001f);
	float2 lightPlanarDirection = AC_LightPos.xz / lightPlanarLength;
	float lightProbe = TX_RainCloudBase.Sample(
		SS_Linear, warpedUV + lightPlanarDirection * 0.018f).r;
	float relief = saturate(0.46f + (density - lightProbe) * 2.1f + (detail - 0.5f) * 0.22f);

	float dayWeight = saturate(AC_LightPos.y * 4.0f + 0.10f);
	float3 dayCloud = lerp(float3(0.10f, 0.12f, 0.14f), float3(0.37f, 0.40f, 0.43f), relief);
	float3 nightCloud = lerp(float3(0.008f, 0.013f, 0.027f), float3(0.055f, 0.072f, 0.115f), relief);
	float3 cloudColor = lerp(nightCloud, dayCloud, dayWeight);
	cloudColor += float3(0.16f, 0.22f, 0.34f) * AC_MoonVisibility * relief * 0.08f;

	float opacity = saturate(0.82f + density * 0.22f);
	return float4(cloudColor, opacity);
}
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float skyDomeY = normalize(Input.vWorldPosition - AC_SpherePosition).y;
	if (skyDomeY < 0.0f)
		return float4(0.0f, 0.0f, 0.0f, 1.0f);

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

