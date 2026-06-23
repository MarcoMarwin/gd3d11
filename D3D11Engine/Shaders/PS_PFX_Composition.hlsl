//--------------------------------------------------------------------------------------
// PostFX Composition Uber Shader
// Merges SAO, HeightFog, and GodRays into a single full-screen pass.
// Permutation macros: COMPOSE_SAO, COMPOSE_HEIGHTFOG, COMPOSE_GODRAYS
//--------------------------------------------------------------------------------------

#if COMPOSE_HEIGHTFOG || COMPOSE_LIGHTSHAFTS || COMPOSE_CONTACT_SHADOWS || COMPOSE_SSGI
#include <AtmosphericScattering.h>
#endif
#if COMPOSE_HEIGHTFOG
#include "DepthReconstruction.h"
#endif

//--------------------------------------------------------------------------------------
// Constant Buffers
//--------------------------------------------------------------------------------------
#if COMPOSE_HEIGHTFOG
cbuffer PFXBuffer : register( b0 )
{
    float4 HF_ProjParams;
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
#endif

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );

Texture2D TX_Backbuffer : register( t0 );

#if COMPOSE_SAO
Texture2D TX_SAO : register( t1 );
#endif

#if COMPOSE_GODRAYS
Texture2D TX_GodRays : register( t2 );
#endif

#if COMPOSE_HEIGHTFOG || COMPOSE_LIGHTSHAFTS || COMPOSE_CONTACT_SHADOWS || COMPOSE_SSGI
Texture2D TX_Depth : register( t3 );
#endif

//--------------------------------------------------------------------------------------
// HeightFog helpers (inlined from PS_PFX_Heightfog.hlsl)
//--------------------------------------------------------------------------------------
#if COMPOSE_HEIGHTFOG
float3 VSPositionFromDepth( float depth, float2 vTexCoord )
{
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord, HF_ProjParams.xy );
}

float ComputeVolumetricFog( float3 cameraToWorldPos, float3 posOriginal )
{
    float cVolFogHeightDensityAtViewer = exp( -HF_HeightFalloff );

    float lenOrig = length( posOriginal - HF_CameraPosition );
    float len = length( cameraToWorldPos );
    float fogInt = len * cVolFogHeightDensityAtViewer;
    const float cSlopeThreshold = 0.01;

    float w = saturate( ( lenOrig - HF_WeightZNear ) / ( HF_WeightZFar - HF_WeightZNear ) );

    if ( abs( cameraToWorldPos.y ) > cSlopeThreshold )
    {
        float t = HF_HeightFalloff * cameraToWorldPos.y * w;
        fogInt *= ( abs( t ) > 0.0001 ? ( ( 1.0 - exp( -t ) ) / t ) : 1.0 );
    }

    return exp( -HF_GlobalDensity * w * fogInt );
}

float FogDither(float2 pixelPosition)
{
    float n1 = frac(52.9829189f * frac(dot(pixelPosition, float2(0.06711056f, 0.00583715f))));
    float n2 = frac(52.9829189f * frac(dot(pixelPosition + 37.17f, float2(0.00583715f, 0.06711056f))));
    return n1 + n2 - 1.0f;
}

float4 ComputeHeightFog( float2 texcoord )
{
    float expDepth = TX_Depth.Sample( SS_Linear, texcoord ).r;
    float3 position = VSPositionFromDepth( expDepth, texcoord );
    position = mul( float4( position, 1 ), HF_InvView ).xyz;
    float3 posOriginal = position;
    position -= HF_CameraPosition;
    position.y -= HF_FogHeight;

    float fog = 1.0f - ComputeVolumetricFog( position, posOriginal );
    float fogDistance = length(posOriginal - HF_CameraPosition);
    float stableFadeEnd = max(HF_WeightZFar, 1000.0f);
    float stableFadeStart = max(HF_WeightZNear, stableFadeEnd * 0.82f);
    float stableWorldFade = smoothstep(stableFadeStart, stableFadeEnd, fogDistance);
    float rainFogBlend = max(saturate(AC_SceneWettness), saturate(AC_RainFXWeight));
    float nightFogBlend = smoothstep(0.0f, 1.0f, saturate(-AC_LightPos.y * 4.0f));
    fog = max(fog, stableWorldFade * rainFogBlend);
    float3 color = ApplyAtmosphericScatteringGround( position, HF_FogColorMod, true, false );
	float nightTimeBlend = nightFogBlend;
	float nightFogBrightness = lerp(1.0f, max(0.0f, AC_NightFogBrightness), saturate(AC_EnableNightAtmosphere));
	float3 nightFogColor = float3(0.12f, 0.18f, 0.27f) * nightFogBrightness;
	color = lerp(color, nightFogColor, nightTimeBlend);

	float dayDarknessFactor = max(1.0f, 2.0f - max(0.0f, AC_LightPos.y));
	float darknessFactor = lerp(dayDarknessFactor, 2.5f, nightTimeBlend);
	float maxFogOpacity = lerp(1.0f, 0.85f, nightTimeBlend);

	return float4(saturate(color / darknessFactor), saturate(fog) * maxFogOpacity);
}
#endif

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
    float2 vTexcoord  : TEXCOORD0;
    float3 vEyeRay    : TEXCOORD1;
    float4 vPosition  : SV_POSITION;
};


#if COMPOSE_LIGHTSHAFTS || COMPOSE_CONTACT_SHADOWS || COMPOSE_SSGI
float GetDepthRaw(float2 uv)
{
    return TX_Depth.SampleLevel(SS_Linear, saturate(uv), 0).r;
}

float IsGeometryPixel(float depth)
{
    return step(0.000001f, depth);
}
#endif

#if COMPOSE_CONTACT_SHADOWS
float ComputeContactShadow(float2 uv)
{
    float center = GetDepthRaw(uv);
    float geom = IsGeometryPixel(center);
    float2 px = max(abs(ddx(uv)) + abs(ddy(uv)), float2(1.0f / 3840.0f, 1.0f / 2160.0f));
    float maxDelta = 0.0f;
    [unroll]
    for (int i = 1; i <= 3; ++i)
    {
        float2 o = px * (float)i * 2.0f;
        float d0 = abs(GetDepthRaw(uv + float2( o.x, 0.0f)) - center);
        float d1 = abs(GetDepthRaw(uv + float2(-o.x, 0.0f)) - center);
        float d2 = abs(GetDepthRaw(uv + float2(0.0f,  o.y)) - center);
        float d3 = abs(GetDepthRaw(uv + float2(0.0f, -o.y)) - center);
        maxDelta = max(maxDelta, max(max(d0, d1), max(d2, d3)) / (float)i);
    }
    float contact = smoothstep(0.00003f, 0.00120f, maxDelta) * geom;
    return 1.0f - contact * saturate(AC_ContactShadowStrength) * 0.22f;
}
#endif

#if COMPOSE_SSGI
float3 ComputeScreenSpaceGILight(float2 uv, float centerDepth)
{
    float geom = IsGeometryPixel(centerDepth);
    float2 px = max(abs(ddx(uv)) + abs(ddy(uv)), float2(1.0f / 3840.0f, 1.0f / 2160.0f));
    float3 bounce = 0.0f;
    float weightSum = 0.0001f;
    const float2 dirs[6] = {
        float2( 1.0f,  0.0f), float2(-1.0f,  0.0f),
        float2( 0.0f,  1.0f), float2( 0.0f, -1.0f),
        float2( 0.7f,  0.7f), float2(-0.7f, -0.7f)
    };
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        float2 suv = saturate(uv + dirs[i] * px * 9.0f);
        float sd = GetDepthRaw(suv);
        float w = saturate(1.0f - abs(sd - centerDepth) * 900.0f) * IsGeometryPixel(sd);
        float3 sampleColor = TX_Backbuffer.SampleLevel(SS_Linear, suv, 0).rgb;
        bounce += sampleColor * w;
        weightSum += w;
    }
    bounce /= weightSum;
    float luma = dot(bounce, float3(0.2126f, 0.7152f, 0.0722f));
    return bounce * saturate(luma * 0.65f) * saturate(AC_ScreenSpaceGIStrength) * geom * 0.18f;
}
#endif

#if COMPOSE_LIGHTSHAFTS
float3 ComputeVolumetricLightShafts(float2 uv, float depth)
{
    float day = saturate(AC_LightPos.y * 2.0f + 0.15f);
    float night = saturate((-AC_LightPos.y + 0.05f) * 1.5f);
    float weather = max(saturate(AC_RainFXWeight), saturate(AC_SceneWettness));
    float atmosphereWeight = saturate(day + night * 0.35f + weather * 0.45f);
    float2 lightCenter = saturate(float2(0.5f, 0.48f) + AC_LightPos.xz * float2(0.38f, -0.24f));
    float2 dir = lightCenter - uv;
    float distToLight = length(dir);
    dir /= max(distToLight, 0.0001f);
    float visibility = 0.0f;
    [unroll]
    for (int i = 1; i <= 6; ++i)
    {
        float2 suv = saturate(uv + dir * (float)i * 0.018f);
        float sd = GetDepthRaw(suv);
        visibility += 1.0f - IsGeometryPixel(sd);
    }
    visibility /= 6.0f;
    float geometryMist = lerp(0.35f, 1.0f, 1.0f - IsGeometryPixel(depth));
    float shaft = visibility * geometryMist * atmosphereWeight * (1.0f - smoothstep(0.15f, 1.05f, distToLight));
    float3 dayColor = float3(1.0f, 0.82f, 0.55f);
    float3 nightColor = float3(0.22f, 0.32f, 0.62f);
    float3 shaftColor = lerp(dayColor, nightColor, night);
    return shaftColor * shaft * saturate(AC_VolumetricLightShaftStrength) * 0.10f;
}
#endif
//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float4 color = TX_Backbuffer.Sample( SS_Linear, Input.vTexcoord );

    // Composition order: SAO (multiply) -> HeightFog (alpha blend) -> GodRays (additive)

#if COMPOSE_SAO
    float ao = TX_SAO.Sample( SS_Linear, Input.vTexcoord ).r;
    color.rgb *= ao;
#endif

#if COMPOSE_CONTACT_SHADOWS || COMPOSE_SSGI || COMPOSE_LIGHTSHAFTS
    float compositionDepth = GetDepthRaw(Input.vTexcoord);
#endif

#if COMPOSE_CONTACT_SHADOWS
    color.rgb *= ComputeContactShadow(Input.vTexcoord);
#endif

#if COMPOSE_SSGI
    color.rgb += ComputeScreenSpaceGILight(Input.vTexcoord, compositionDepth);
#endif

#if COMPOSE_HEIGHTFOG
    float4 fog = ComputeHeightFog( Input.vTexcoord );
    color.rgb = lerp( color.rgb, fog.rgb, fog.a );
    float nightTimeBlend = smoothstep(0.0f, 1.0f, saturate(-AC_LightPos.y * 4.0f));
    float ditherStrength = lerp(1.5f, 2.0f, nightTimeBlend) / 255.0f;
    color.rgb = saturate(color.rgb + FogDither(Input.vPosition.xy) * fog.a * ditherStrength);
#endif

#if COMPOSE_GODRAYS
    float3 godrays = TX_GodRays.Sample( SS_Linear, Input.vTexcoord ).rgb;
    color.rgb += godrays;
#endif

#if COMPOSE_LIGHTSHAFTS
    color.rgb += ComputeVolumetricLightShafts(Input.vTexcoord, compositionDepth);
#endif

    return color;
}
