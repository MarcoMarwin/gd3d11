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
Texture2D	TX_VDBCloudAtlas : register( t4 );


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
    float moonMask = boundsMask * textureMask * GetRainSkyVisibility();
    return moonTexture.rgb * moonMask;
}

float VDBCloudHash(float2 cell)
{
    return frac(sin(dot(cell, float2(127.1f, 311.7f))) * 43758.5453f);
}

float4 SampleVDBCloudLayer(float2 planePosition, float layerScale, float2 wind, float seed)
{
    float2 tiledPosition = planePosition * layerScale + wind + seed;
    float2 cell = floor(tiledPosition);
    float2 localUV = frac(tiledPosition);

    float occupancyHash = VDBCloudHash(cell + seed * 11.0f);
    float cellMask = smoothstep(0.48f, 0.78f, occupancyHash);
    float shapeHash = VDBCloudHash(cell + seed * 17.0f);
    float variant = min(9.0f, floor(shapeHash * 10.0f));

    // Keep the atlas at a normal local-cloud size. The puff window leaves empty
    // sky between cells, while the source volume itself remains dense inside.
    float2 puffUV = (localUV - 0.5f) * 1.36f + 0.5f;
    float2 inside = step(0.0f, puffUV) * step(puffUV, 1.0f);
    float boundsMask = inside.x * inside.y;
    float2 edge = abs(localUV - 0.5f) * 2.0f;
    float edgeMask = 1.0f - smoothstep(0.70f, 1.0f, max(edge.x, edge.y));

    float flipHash = VDBCloudHash(cell.yx + seed * 43.0f);
    if (flipHash > 0.5f)
        puffUV.x = 1.0f - puffUV.x;

    float2 atlasCell = float2(fmod(variant, 5.0f), floor(variant / 5.0f));
    float2 atlasUV = (atlasCell + clamp(puffUV, 0.002f, 0.998f)) / float2(5.0f, 2.0f);
    float4 sampleCloud = TX_VDBCloudAtlas.Sample(SS_Linear, atlasUV);
    float denseMask = smoothstep(0.18f, 0.50f, sampleCloud.a) * boundsMask * edgeMask * cellMask;
    return float4(sampleCloud.rgb, saturate(denseMask));
}

float4 RenderVDBClouds(float3 worldPosition)
{
    float3 skyDirection = normalize(worldPosition - AC_SpherePosition);
    float horizonMask = smoothstep(0.015f, 0.14f, skyDirection.y);
    float projectionHeight = max(0.18f, skyDirection.y);
    float2 cameraParallax = AC_WorldCameraPos.xz * 0.0000012f;
    float2 planePosition = skyDirection.xz / projectionHeight * 0.72f + cameraParallax;

    float2 wind0 = float2(0.000070f, 0.000026f) * AC_Time;
    float2 wind1 = float2(-0.000040f, 0.000052f) * AC_Time;
    float2 wind2 = float2(0.000024f, -0.000034f) * AC_Time;
    float4 layer0 = SampleVDBCloudLayer(planePosition, 1.20f, wind0, 0.31f);
    float4 layer1 = SampleVDBCloudLayer(planePosition + 2.41f, 1.72f, wind1, 0.67f);
    float4 layer2 = SampleVDBCloudLayer(planePosition - 1.37f, 2.18f, wind2, 0.93f);

    float coverage0 = saturate(layer0.a * 0.92f);
    float coverage1 = saturate(layer1.a * 0.72f);
    float coverage2 = saturate(layer2.a * 0.48f);
    float coverage = 1.0f - (1.0f - coverage0) * (1.0f - coverage1) * (1.0f - coverage2);
    float3 sourceColor = (layer0.rgb * coverage0 + layer1.rgb * coverage1 + layer2.rgb * coverage2)
        / max(0.001f, coverage0 + coverage1 + coverage2);
    float relief = saturate(dot(sourceColor, float3(0.2126f, 0.7152f, 0.0722f)) * 1.45f);

    float dayWeight = saturate(AC_LightPos.y * 4.0f + 0.12f);
    float rainWeight = smoothstep(0.02f, 0.70f, saturate(AC_RainFXWeight));
    float3 nightColor = lerp(float3(0.010f, 0.018f, 0.034f), float3(0.10f, 0.14f, 0.22f), relief);
    nightColor += float3(0.10f, 0.14f, 0.24f) * AC_MoonVisibility * relief * 0.20f;
    float3 dayColor = lerp(float3(0.44f, 0.47f, 0.52f), float3(1.04f, 1.05f, 1.04f), relief);
    float3 rainColor = lerp(float3(0.10f, 0.12f, 0.16f), float3(0.35f, 0.39f, 0.45f), relief);
    float3 cloudColor = lerp(nightColor, dayColor, dayWeight);
    cloudColor = lerp(cloudColor, rainColor, rainWeight);

    float opacity = saturate(coverage * horizonMask * lerp(0.88f, 0.95f, rainWeight));
    return float4(cloudColor, opacity);
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

	float opacity = saturate((0.74f + structure * 0.18f) * horizonMask);
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
	
	float4 vdbClouds = RenderVDBClouds(Input.vWorldPosition);
	atmoColor = lerp(atmoColor, vdbClouds.rgb, vdbClouds.a);
	// Apply stars
	atmoColor += night * 0.4f;
	
	atmoColor = saturate(atmoColor + AtmosphereDither(Input.vPosition.xy) * (GetNightWeight() * 1.5f / 255.0f));
	return float4(atmoColor,1);
}
